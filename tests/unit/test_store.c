/*
 * Testy úložiště bloků FS (fs/store.h): společná sada proti mock i dir,
 * pak chování specifické pro adresář (zámek rootu, úklid tmp/, karanténa
 * poškozených bloků, plný disk přes RLIMIT_FSIZE).
 */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _XOPEN_SOURCE 700              /* nftw, rand_r, mkdtemp, pwrite */
#endif
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/store.h"
#include "proto/errors.h"
#include "proto/protocol.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

static unsigned g_seed = 4242;

static uint8_t *rnd_block(uint32_t n, struct kivfs_digest *h)
{
    uint8_t *p = malloc(n ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "malloc\n");
        exit(2);
    }
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(rand_r(&g_seed) >> 7);
    if (h)
        kivfs_sha256(p, n, h);
    return p;
}

static int g_logs;
static void count_log(void *ud, const char *msg)
{
    (void)ud;
    (void)msg;
    g_logs++;
}

/* ------------------------------------------------------------------ */
/* Společná sada: musí platit pro každý backend                       */
/* ------------------------------------------------------------------ */
static void common_suite(struct kivfs_store *s, const char *name)
{
    const struct kivfs_store_ops *o = s->ops;
    struct kivfs_digest h, h2, bogus;
    uint32_t len = 0;
    uint8_t *a = rnd_block(100000, &h);
    uint8_t *buf = malloc(KIVFS_FS_BLOCK_MAX);

    fprintf(stderr, "-- společná sada: %s\n", name);
    CHECK(buf != NULL);

    /* uložení a čtení zpět */
    CHECK(o->has(s, &h, &len) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->put(s, &h, a, 100000) == KIVFS_STATUS_OK);
    CHECK(o->has(s, &h, &len) == KIVFS_STATUS_OK && len == 100000);
    CHECK(o->has(s, &h, NULL) == KIVFS_STATUS_OK);
    len = 0;
    CHECK(o->get(s, &h, buf, KIVFS_FS_BLOCK_MAX, &len, 1) == KIVFS_STATUS_OK);
    CHECK(len == 100000 && memcmp(buf, a, 100000) == 0);
    CHECK(o->get(s, &h, buf, 100000, &len, 0) == KIVFS_STATUS_OK);   /* přesný buffer */

    /* idempotence */
    CHECK(o->put(s, &h, a, 100000) == KIVFS_STATUS_OK);
    CHECK(o->has(s, &h, &len) == KIVFS_STATUS_OK && len == 100000);

    /* malý buffer */
    CHECK(o->get(s, &h, buf, 99999, &len, 0) == KIVFS_STATUS_INTERNAL);

    /* podvržený otisk: data jiná než otisk -> nic se neuloží */
    memset(&bogus, 0xAB, sizeof bogus);
    CHECK(o->put(s, &bogus, a, 100000) == KIVFS_STATUS_MALFORMED);
    CHECK(o->has(s, &bogus, NULL) == KIVFS_STATUS_NOT_FOUND);
    a[5] ^= 1;                                  /* data pod cizím (platným) otiskem */
    CHECK(o->put(s, &h, a, 100000) == KIVFS_STATUS_MALFORMED);
    a[5] ^= 1;
    CHECK(o->get(s, &h, buf, KIVFS_FS_BLOCK_MAX, &len, 1) == KIVFS_STATUS_OK
          && memcmp(buf, a, 100000) == 0);      /* původní obsah nedotčen */

    /* délky mimo rozsah */
    CHECK(o->put(s, &h, a, 0) == KIVFS_STATUS_MALFORMED);
    CHECK(o->put(s, &h, NULL, 10) == KIVFS_STATUS_MALFORMED);
    CHECK(o->put(s, &h, a, KIVFS_FS_BLOCK_MAX + 1) == KIVFS_STATUS_MALFORMED);

    /* neexistující */
    CHECK(o->get(s, &bogus, buf, KIVFS_FS_BLOCK_MAX, &len, 1) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->remove(s, &bogus) == KIVFS_STATUS_NOT_FOUND);

    /* nejmenší a největší blok */
    {
        uint8_t one = 0x42;
        kivfs_sha256(&one, 1, &h2);
        CHECK(o->put(s, &h2, &one, 1) == KIVFS_STATUS_OK);
        CHECK(o->get(s, &h2, buf, 1, &len, 1) == KIVFS_STATUS_OK && len == 1 && buf[0] == 0x42);
        CHECK(o->remove(s, &h2) == KIVFS_STATUS_OK);
    }
    {
        uint8_t *big = rnd_block(KIVFS_FS_BLOCK_MAX, &h2);
        CHECK(o->put(s, &h2, big, KIVFS_FS_BLOCK_MAX) == KIVFS_STATUS_OK);
        CHECK(o->get(s, &h2, buf, KIVFS_FS_BLOCK_MAX, &len, 1) == KIVFS_STATUS_OK
              && len == KIVFS_FS_BLOCK_MAX && memcmp(buf, big, len) == 0);
        CHECK(o->remove(s, &h2) == KIVFS_STATUS_OK);
        free(big);
    }

    /* více bloků vedle sebe (různé délky), pak smazání každého druhého */
    {
        enum { N = 40 };
        struct kivfs_digest hs[N];
        uint8_t *bs[N];
        uint32_t ls[N];
        for (int i = 0; i < N; i++) {
            ls[i] = 1 + (uint32_t)(rand_r(&g_seed) % 70000);
            bs[i] = rnd_block(ls[i], &hs[i]);
            CHECK(o->put(s, &hs[i], bs[i], ls[i]) == KIVFS_STATUS_OK);
        }
        for (int i = 0; i < N; i += 2)
            CHECK(o->remove(s, &hs[i]) == KIVFS_STATUS_OK);
        for (int i = 0; i < N; i++) {
            int16_t st = o->get(s, &hs[i], buf, KIVFS_FS_BLOCK_MAX, &len, 1);
            if (i % 2 == 0)
                CHECK(st == KIVFS_STATUS_NOT_FOUND);
            else
                CHECK(st == KIVFS_STATUS_OK && len == ls[i] && memcmp(buf, bs[i], len) == 0);
            free(bs[i]);
        }
    }

    /* remove a znovu put */
    CHECK(o->remove(s, &h) == KIVFS_STATUS_OK);
    CHECK(o->has(s, &h, NULL) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->remove(s, &h) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->put(s, &h, a, 100000) == KIVFS_STATUS_OK);
    CHECK(o->has(s, &h, &len) == KIVFS_STATUS_OK && len == 100000);

    free(buf);
    free(a);
}

/* ------------------------------------------------------------------ */
/* Pomůcky pro dir                                                    */
/* ------------------------------------------------------------------ */
static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return remove(p);
}

static void rm_rf(const char *path)
{
    nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static int count_files(const char *dir)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "ls -A '%s' | wc -l", dir);
    FILE *f = popen(cmd, "r");
    int n = -1;
    if (f) {
        if (fscanf(f, "%d", &n) != 1)
            n = -1;
        pclose(f);
    }
    return n;
}

static void blk_path(const char *root, const struct kivfs_digest *h, char *out, size_t n)
{
    char hex[KIVFS_DIGEST_HEX_LEN + 1];
    kivfs_digest_hex(h, hex);
    snprintf(out, n, "%s/blocks/%.2s/%.2s/%s", root, hex, hex + 2, hex);
}

static struct kivfs_store *open_dir(const char *root, int fsync_on)
{
    struct kivfs_store_dir_cfg cfg = { root, fsync_on, count_log, NULL };
    struct kivfs_store *s = NULL;
    char err[256] = "";
    int rc = kivfs_store_dir_open(&cfg, &s, err, sizeof err);
    if (rc != KIVFS_OK) {
        fprintf(stderr, "open_dir(%s): %d %s\n", root, rc, err);
        return NULL;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Specifické pro dir                                                 */
/* ------------------------------------------------------------------ */
static void test_dir_open_errors(const char *base)
{
    struct kivfs_store *s = NULL;
    struct kivfs_store_dir_cfg cfg = { NULL, 1, NULL, NULL };
    char err[256];

    err[0] = '\0';
    CHECK(kivfs_store_dir_open(&cfg, &s, err, sizeof err) == KIVFS_ECONFIG && err[0]);
    cfg.root = "";
    CHECK(kivfs_store_dir_open(&cfg, &s, err, sizeof err) == KIVFS_ECONFIG);
    cfg.root = "relativni/cesta";
    CHECK(kivfs_store_dir_open(&cfg, &s, err, sizeof err) == KIVFS_ECONFIG);
    CHECK(kivfs_store_dir_open(&cfg, &s, NULL, 0) == KIVFS_ECONFIG);   /* err smí být NULL */

    /* root je soubor */
    char path[512];
    snprintf(path, sizeof path, "%s/soubor", base);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) fclose(f);
    cfg.root = path;
    CHECK(kivfs_store_dir_open(&cfg, &s, err, sizeof err) == KIVFS_ECONFIG);
}

static void test_dir_layout_and_persistence(const char *base)
{
    char root[512], p[700], root_slash[520];
    struct kivfs_digest h;
    uint32_t len;
    uint8_t *a = rnd_block(5000, &h);
    uint8_t buf[5000];
    struct stat st;

    /* root s neexistujícími rodiči a koncovým lomítkem */
    snprintf(root, sizeof root, "%s/a/b/c", base);
    snprintf(root_slash, sizeof root_slash, "%s/", root);
    struct kivfs_store *s = open_dir(root_slash, 1);
    CHECK(s != NULL);
    if (!s) { free(a); return; }
    CHECK(s->ops->put(s, &h, a, 5000) == KIVFS_STATUS_OK);
    blk_path(root, &h, p, sizeof p);
    CHECK(stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 5000);
    CHECK((st.st_mode & 0777) == 0600);
    snprintf(p, sizeof p, "%s/tmp", root);
    CHECK(count_files(p) == 0);                 /* po put nic nezbylo */

    /* druhý proces (druhé otevření) téhož rootu se odmítne */
    {
        struct kivfs_store *s2 = NULL;
        struct kivfs_store_dir_cfg cfg = { root, 1, NULL, NULL };
        char err[256] = "";
        CHECK(kivfs_store_dir_open(&cfg, &s2, err, sizeof err) == KIVFS_ECONFIG);
        CHECK(strstr(err, "jiný proces") != NULL);
    }
    s->ops->close(s);

    /* data přežijí zavření; zbytek v tmp/ zmizí */
    snprintf(p, sizeof p, "%s/tmp/zbytek.123", root);
    FILE *f = fopen(p, "w");
    CHECK(f != NULL);
    if (f) { fputs("půlka bloku", f); fclose(f); }
    g_logs = 0;
    s = open_dir(root, 1);
    CHECK(s != NULL);
    if (!s) { free(a); return; }
    CHECK(access(p, F_OK) != 0);
    CHECK(g_logs > 0);
    CHECK(s->ops->get(s, &h, buf, sizeof buf, &len, 1) == KIVFS_STATUS_OK
          && len == 5000 && memcmp(buf, a, 5000) == 0);
    s->ops->close(s);
    free(a);
}

static void test_dir_corruption(const char *base)
{
    char root[512], p[700], bad[600];
    struct kivfs_digest h;
    uint32_t len;
    uint8_t *a = rnd_block(8000, &h);
    uint8_t *buf = malloc(KIVFS_FS_BLOCK_MAX);

    snprintf(root, sizeof root, "%s/corrupt", base);
    snprintf(bad, sizeof bad, "%s/bad", root);
    struct kivfs_store *s = open_dir(root, 0);   /* zároveň zkouška fsync = no */
    CHECK(s != NULL && buf != NULL);
    if (!s || !buf) { free(a); free(buf); return; }
    const struct kivfs_store_ops *o = s->ops;
    blk_path(root, &h, p, sizeof p);

    /* 1) překlopený bajt: bez verify projde, s verify karanténa + NOT_FOUND */
    CHECK(o->put(s, &h, a, 8000) == KIVFS_STATUS_OK);
    {
        int fd = open(p, O_WRONLY);
        uint8_t x = (uint8_t)(a[100] ^ 0xFF);
        CHECK(fd >= 0 && pwrite(fd, &x, 1, 100) == 1);
        if (fd >= 0) close(fd);
    }
    CHECK(o->get(s, &h, buf, KIVFS_FS_BLOCK_MAX, &len, 0) == KIVFS_STATUS_OK && len == 8000);
    g_logs = 0;
    CHECK(o->get(s, &h, buf, KIVFS_FS_BLOCK_MAX, &len, 1) == KIVFS_STATUS_NOT_FOUND);
    CHECK(g_logs > 0);
    CHECK(access(p, F_OK) != 0);
    CHECK(count_files(bad) == 1);
    CHECK(o->has(s, &h, NULL) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->put(s, &h, a, 8000) == KIVFS_STATUS_OK);        /* oprava novým put */
    CHECK(o->get(s, &h, buf, KIVFS_FS_BLOCK_MAX, &len, 1) == KIVFS_STATUS_OK
          && memcmp(buf, a, 8000) == 0);

    /* 2) zkrácený soubor (jiná délka) -> put ho nahradí */
    CHECK(truncate(p, 3000) == 0);
    CHECK(o->has(s, &h, &len) == KIVFS_STATUS_OK && len == 3000);
    CHECK(o->put(s, &h, a, 8000) == KIVFS_STATUS_OK);
    CHECK(o->has(s, &h, &len) == KIVFS_STATUS_OK && len == 8000);

    /* 3) prázdný soubor = nemožná délka -> karanténa i bez verify */
    CHECK(truncate(p, 0) == 0);
    CHECK(o->get(s, &h, buf, KIVFS_FS_BLOCK_MAX, &len, 0) == KIVFS_STATUS_NOT_FOUND);
    CHECK(count_files(bad) == 2);

    s->ops->close(s);
    free(a);
    free(buf);
}

/* Plný disk: RLIMIT_FSIZE + ignorovaný SIGXFSZ -> write vrátí EFBIG. */
static void test_dir_no_space(const char *base)
{
    char root[512], p[600];
    struct kivfs_digest h;
    uint8_t *a = rnd_block(200000, &h);
    struct rlimit old, lim;

    snprintf(root, sizeof root, "%s/full", base);
    struct kivfs_store *s = open_dir(root, 1);
    CHECK(s != NULL);
    if (!s) { free(a); return; }

    CHECK(getrlimit(RLIMIT_FSIZE, &old) == 0);
    void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
    lim = old;
    lim.rlim_cur = 4096;
    CHECK(setrlimit(RLIMIT_FSIZE, &lim) == 0);

    CHECK(s->ops->put(s, &h, a, 200000) == KIVFS_STATUS_NO_SPACE);

    CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
    signal(SIGXFSZ, prev);

    CHECK(s->ops->has(s, &h, NULL) == KIVFS_STATUS_NOT_FOUND);
    snprintf(p, sizeof p, "%s/tmp", root);
    CHECK(count_files(p) == 0);                 /* rozpracovaný soubor smazán */
    CHECK(s->ops->put(s, &h, a, 200000) == KIVFS_STATUS_OK);   /* po uvolnění projde */

    s->ops->close(s);
    free(a);
}

int main(void)
{
    struct kivfs_store *s = NULL;
    char base[] = "/tmp/kivfs_store_XXXXXX";
    char root[600];

    /* mock */
    CHECK(kivfs_store_mock_open(&s) == KIVFS_OK);
    if (s) {
        common_suite(s, "mock");
        s->ops->close(s);
    }

    /* dir */
    if (mkdtemp(base) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(root, sizeof root, "%s/common", base);
    s = open_dir(root, 1);
    CHECK(s != NULL);
    if (s) {
        common_suite(s, "dir");
        s->ops->close(s);
    }
    test_dir_open_errors(base);
    test_dir_layout_and_persistence(base);
    test_dir_corruption(base);
    test_dir_no_space(base);

    rm_rf(base);
    printf("store: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
