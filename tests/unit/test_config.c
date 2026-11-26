/*
 * Testy konfigurace: INI parser (config.h) a pohled modulu (modconf.h).
 * Každý případ si zapíše dočasný soubor. Na konci se načtou i všechny
 * šablony z conf/ pro všech 5 modulů (KIVFS_SOURCE_DIR z CMake).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/config.h"
#include "proto/errors.h"
#include "core/modconf.h"

#ifndef KIVFS_SOURCE_DIR
#  define KIVFS_SOURCE_DIR "."
#endif

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

static char g_path[64];
static char g_err[512];

static const char *write_tmp(const char *text)
{
    strcpy(g_path, "/tmp/kivfs_cfg_XXXXXX");
    int fd = mkstemp(g_path);
    if (fd < 0)
        return NULL;
    size_t n = strlen(text);
    if (write(fd, text, n) != (ssize_t)n) { }
    close(fd);
    return g_path;
}

/* Načte text; vrátí rc a config v *out (nebo NULL). */
static int load_text(const char *text, struct kivfs_config **out)
{
    g_err[0] = '\0';
    const char *p = write_tmp(text);
    int rc = kivfs_config_load(p, out, g_err, sizeof g_err);
    unlink(p);
    return rc;
}

/* Načte text a sestaví modconf; vrátí rc (chyba config nebo modconf). */
static int modconf_text(const char *text, enum kivfs_module m, const char *const *extra,
                        struct kivfs_modconf *mc)
{
    struct kivfs_config *c;
    int rc = load_text(text, &c);
    if (rc != KIVFS_OK)
        return rc;
    rc = kivfs_modconf_from(c, m, extra, mc, g_err, sizeof g_err);
    kivfs_config_free(c);
    return rc;
}

/* Chyba s očekávaným číslem řádku a kouskem textu v hlášce. */
#define EXPECT_ERR(rc, line_str, needle)                                    \
    do {                                                                    \
        CHECK((rc) == KIVFS_ECONFIG);                                       \
        CHECK(strstr(g_err, line_str) != NULL);                             \
        CHECK(strstr(g_err, needle) != NULL);                               \
        if ((rc) != KIVFS_ECONFIG || !strstr(g_err, line_str) || !strstr(g_err, needle)) \
            fprintf(stderr, "     err = \"%s\"\n", g_err);                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

static int count_keys(const char *k, const char *v, int line, void *ud)
{
    (void)k; (void)v; (void)line;
    (*(int *)ud)++;
    return 0;
}

static void test_parser_ok(void)
{
    struct kivfs_config *c;
    int rc = load_text(
        "; komentář\n"
        "# taky komentář\n"
        "\n"
        "[a]\n"
        "  x =  1  \n"
        "y=hodnota ; komentář za hodnotou\n"
        "url = host#bez-mezery-neni-komentar\n"
        "empty =\n"
        "[b] ; komentář za sekcí\n"
        "x = 2\r\n", &c);
    CHECK(rc == KIVFS_OK);
    if (rc != KIVFS_OK) { fprintf(stderr, "     %s\n", g_err); return; }
    CHECK(strcmp(kivfs_config_get(c, "a", "x"), "1") == 0);
    CHECK(strcmp(kivfs_config_get(c, "a", "y"), "hodnota") == 0);
    CHECK(strcmp(kivfs_config_get(c, "a", "url"), "host#bez-mezery-neni-komentar") == 0);
    CHECK(strcmp(kivfs_config_get(c, "a", "empty"), "") == 0);
    CHECK(strcmp(kivfs_config_get(c, "b", "x"), "2") == 0);
    CHECK(kivfs_config_get(c, "a", "nic") == NULL);
    CHECK(kivfs_config_get(c, "nic", "x") == NULL);
    CHECK(kivfs_config_line(c, "a", "y") == 6);
    CHECK(kivfs_config_line(c, "b", NULL) == 9);
    int n = 0;
    kivfs_config_foreach(c, "a", count_keys, &n);
    CHECK(n == 4);
    kivfs_config_free(c);
}

static void test_parser_errors(void)
{
    struct kivfs_config *c = (void *)1;
    int rc;

    rc = load_text("x = 1\n", &c);
    EXPECT_ERR(rc, ":1:", "před první");
    CHECK(c == NULL);
    rc = load_text("[a]\nbez_rovnitka\n", &c);
    EXPECT_ERR(rc, ":2:", "klíč = hodnota");
    rc = load_text("[a]\nx = 1\nx = 2\n", &c);
    EXPECT_ERR(rc, ":3:", "řádku 2");
    rc = load_text("[a]\n[b]\n[a]\n", &c);
    EXPECT_ERR(rc, ":3:", "řádku 1");
    rc = load_text("[a\n", &c);
    EXPECT_ERR(rc, ":1:", "]");
    rc = load_text("[a b]\n", &c);
    EXPECT_ERR(rc, ":1:", "název sekce");
    rc = load_text("[a]\nk lic = 1\n", &c);
    EXPECT_ERR(rc, ":2:", "název klíče");

    char big[KIVFS_CONFIG_LINE_MAX + 64];
    strcpy(big, "[a]\nx = ");
    size_t n = strlen(big);
    memset(big + n, 'v', sizeof big - n - 2);
    big[sizeof big - 2] = '\n';
    big[sizeof big - 1] = '\0';
    rc = load_text(big, &c);
    EXPECT_ERR(rc, ":2:", "delší");

    rc = kivfs_config_load("/neexistuje/kivfs.ini", &c, g_err, sizeof g_err);
    CHECK(rc == KIVFS_EIO);
}

/* ------------------------------------------------------------------ */
/* modconf                                                             */
/* ------------------------------------------------------------------ */

#define CLUSTER  "[cluster]\nlocal = node2\nnode1 = a\nnode3 = c:40002\nnode2 = [::1]\n"
#define MODULES  "[auth]\nip = h-auth\nport = 1\n"     \
                 "[sync]\nip = h-sync\nport = 2\n"     \
                 "[vfs]\nip = h-vfs\nport = 3\n"       \
                 "[db]\nip = h-db\nport = 4\n"         \
                 "[fs]\nip = h-fs\nport = 5\n"

static void test_modconf_ok(void)
{
    struct kivfs_modconf mc;
    int rc = modconf_text(CLUSTER MODULES, KIVFS_MOD_VFS, NULL, &mc);
    CHECK(rc == KIVFS_OK);
    if (rc != KIVFS_OK) { fprintf(stderr, "     %s\n", g_err); return; }

    CHECK(mc.self == KIVFS_MOD_VFS && mc.local_node == 2);
    CHECK(strcmp(mc.advertise.host, "h-vfs") == 0 && mc.advertise.port == 3);
    CHECK(mc.has_dep[KIVFS_MOD_DB] && mc.has_dep[KIVFS_MOD_FS]);
    CHECK(!mc.has_dep[KIVFS_MOD_SYNC] && !mc.has_dep[KIVFS_MOD_AUTH]);
    CHECK(strcmp(mc.deps[KIVFS_MOD_DB].host, "h-db") == 0 && mc.deps[KIVFS_MOD_DB].port == 4);

    /* uzly seřazené podle id, výchozí port = [sync] port, IPv6 v [] */
    CHECK(mc.node_count == 3);
    CHECK(mc.nodes[0].id == 1 && mc.nodes[1].id == 2 && mc.nodes[2].id == 3);
    CHECK(strcmp(mc.nodes[0].sync.host, "a") == 0 && mc.nodes[0].sync.port == 2);
    CHECK(strcmp(mc.nodes[1].sync.host, "::1") == 0 && mc.nodes[1].sync.port == 2);
    CHECK(mc.nodes[2].sync.port == 40002);
    CHECK(kivfs_modconf_node(&mc, 3) == &mc.nodes[2]);
    CHECK(kivfs_modconf_node(&mc, 4) == NULL);

    /* výchozí hodnoty a konfigurace reactoru */
    struct kivfs_reactor_config rc_cfg;
    kivfs_modconf_reactor_cfg(&mc, &rc_cfg);
    CHECK(rc_cfg.port == 3 && rc_cfg.bind_addr == NULL);
    CHECK(rc_cfg.max_conns == 256 && rc_cfg.rx_timeout_ms == 10000);
    CHECK(rc_cfg.max_out_conns == 0 && rc_cfg.connect_timeout_ms == 0);

    /* závislosti ostatních modulů */
    CHECK(modconf_text(CLUSTER MODULES, KIVFS_MOD_AUTH, NULL, &mc) == KIVFS_OK);
    CHECK(mc.has_dep[KIVFS_MOD_SYNC] && !mc.has_dep[KIVFS_MOD_VFS]);
    CHECK(modconf_text(CLUSTER MODULES, KIVFS_MOD_SYNC, NULL, &mc) == KIVFS_OK);
    CHECK(mc.has_dep[KIVFS_MOD_VFS] && mc.has_dep[KIVFS_MOD_DB] && !mc.has_dep[KIVFS_MOD_SYNC]);
    CHECK(modconf_text(CLUSTER MODULES, KIVFS_MOD_DB, NULL, &mc) == KIVFS_OK);
    for (int m = 0; m < KIVFS_MOD_COUNT; m++)
        CHECK(mc.has_dep[m] == (m == KIVFS_MOD_SYNC));      /* DB -> SYNC (SUBMIT) */
    CHECK(modconf_text(CLUSTER MODULES, KIVFS_MOD_FS, NULL, &mc) == KIVFS_OK);
    for (int m = 0; m < KIVFS_MOD_COUNT; m++)
        CHECK(!mc.has_dep[m]);

    /* volitelné klíče + extra klíče modulu */
    static const char *const db_keys[] = { "mysql_user", NULL };
    rc = modconf_text(CLUSTER
                      "[sync]\nip = s\nport = 2\n"
                      "[db]\nip = h-db\nport = 4\nbind = 10.0.0.1\nmax_conns = 7\n"
                      "max_out_conns = 3\nmax_pending = 2\nconnect_timeout_ms = 900\n"
                      "rx_timeout_ms = 0\nmysql_user = kivfs\n",
                      KIVFS_MOD_DB, db_keys, &mc);
    CHECK(rc == KIVFS_OK);
    if (rc != KIVFS_OK) fprintf(stderr, "     %s\n", g_err);
    kivfs_modconf_reactor_cfg(&mc, &rc_cfg);
    CHECK(rc_cfg.bind_addr != NULL && strcmp(rc_cfg.bind_addr, "10.0.0.1") == 0);
    CHECK(rc_cfg.max_conns == 7 && rc_cfg.max_out_conns == 3 && rc_cfg.max_pending == 2);
    CHECK(rc_cfg.connect_timeout_ms == 900 && rc_cfg.rx_timeout_ms == 0);
}

static void test_modconf_errors(void)
{
    struct kivfs_modconf mc;
    int rc;

    /* neznámý klíč ve vlastní sekci, ale cizí sekce se nekontroluje */
    rc = modconf_text(CLUSTER MODULES "", KIVFS_MOD_SYNC, NULL, &mc);
    CHECK(rc == KIVFS_OK);
    rc = modconf_text(CLUSTER "[sync]\nip = s\nprot = 2\n", KIVFS_MOD_SYNC, NULL, &mc);
    EXPECT_ERR(rc, ":8:", "neznámý klíč 'prot'");
    rc = modconf_text(CLUSTER "[sync]\nip = s\nport = 2\n[db]\nip = d\nport = 4\nmysql_user = x\n",
                      KIVFS_MOD_SYNC, NULL, &mc);
    EXPECT_ERR(rc, "", "[vfs]");          /* sync potřebuje vfs; db klíče ho nezajímají */
    rc = modconf_text(CLUSTER "[sync]\nip = s\nport = 2\n[vfs]\nip = v\nport = 3\n"
                      "[db]\nip = d\nport = 4\nmysql_user = x\n", KIVFS_MOD_SYNC, NULL, &mc);
    CHECK(rc == KIVFS_OK);

    /* neznámá sekce */
    rc = modconf_text(CLUSTER MODULES "[snyc]\n", KIVFS_MOD_DB, NULL, &mc);
    EXPECT_ERR(rc, ":21:", "neznámá sekce [snyc]");

    /* chybějící věci */
    rc = modconf_text(CLUSTER "[sync]\nip = s\nport = 2\n", KIVFS_MOD_DB, NULL, &mc);
    EXPECT_ERR(rc, "", "chybí sekce [db]");
    rc = modconf_text(CLUSTER "[db]\nip = d\nport = 4\n", KIVFS_MOD_DB, NULL, &mc);
    EXPECT_ERR(rc, "", "chybí sekce [sync]");  /* DB volá Sync (SUBMIT) */
    rc = modconf_text(CLUSTER "[sync]\nport = 2\n[vfs]\nip=v\nport=3\n", KIVFS_MOD_SYNC, NULL, &mc);
    EXPECT_ERR(rc, ":6:", "'ip'");
    rc = modconf_text("[fs]\nip = d\nport = 4\n", KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, "", "[cluster]");
    rc = modconf_text("[cluster]\nnode1 = a:1\n[fs]\nip = d\nport = 4\n", KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":1:", "'local'");
    rc = modconf_text("[cluster]\nlocal = node1\n[fs]\nip = d\nport = 4\n", KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":1:", "žádný uzel");

    /* špatné hodnoty */
    rc = modconf_text(CLUSTER "[sync]\nip = s\nport = 70000\n[vfs]\nip=v\nport=3\n",
                      KIVFS_MOD_SYNC, NULL, &mc);
    EXPECT_ERR(rc, ":8:", "1..65535");
    rc = modconf_text(CLUSTER "[sync]\nip = s\nport = 2x\n[vfs]\nip=v\nport=3\n",
                      KIVFS_MOD_SYNC, NULL, &mc);
    EXPECT_ERR(rc, ":8:", "není číslo");
    rc = modconf_text(CLUSTER "[fs]\nip = d\nport = 4\nmax_conns = -1\n", KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":9:", "max_conns");

    /* uzly */
    rc = modconf_text("[cluster]\nlocal = node4\nnode1 = a:1\n[fs]\nip=d\nport=4\n",
                      KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":2:", "node4");
    rc = modconf_text("[cluster]\nlocal = node1\nnode0 = a:1\n[fs]\nip=d\nport=4\n",
                      KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":3:", "node0");
    rc = modconf_text("[cluster]\nlocal = node1\nnode1 = a:1\nnode01 = b:1\n[fs]\nip=d\nport=4\n",
                      KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":4:", "node01");
    rc = modconf_text("[cluster]\nlocal = node1\nnodes = a\n[fs]\nip=d\nport=4\n",
                      KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":3:", "neznámý klíč 'nodes'");
    rc = modconf_text("[cluster]\nlocal = node1\nnode1 = a\n[fs]\nip=d\nport=4\n",
                      KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":3:", "chybí port");      /* bez portu a bez [sync] */
    rc = modconf_text("[cluster]\nlocal = node1\nnode1 = ::1:5\n[fs]\nip=d\nport=4\n",
                      KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":3:", "IPv6");
    rc = modconf_text("[cluster]\nlocal = node1\nnode1 = a:0\n[fs]\nip=d\nport=4\n",
                      KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, ":3:", "neplatná adresa");

    char many[4096] = "[cluster]\nlocal = node1\n";
    for (int i = 1; i <= KIVFS_MAX_NODES + 1; i++) {
        char l[64];
        snprintf(l, sizeof l, "node%d = h%d:1\n", i, i);
        strcat(many, l);
    }
    strcat(many, "[fs]\nip=d\nport=4\n");
    rc = modconf_text(many, KIVFS_MOD_FS, NULL, &mc);
    EXPECT_ERR(rc, "", "uzlů");

    CHECK(modconf_text(CLUSTER MODULES, KIVFS_MOD_CLIENT, NULL, &mc) == KIVFS_EINVAL);
}

/* Všechny šablony z conf/ musí projít pro všech 5 modulů. */
static void test_templates(void)
{
    static const char *files[] = {
        "3nodes/node1.ini", "3nodes/node2.ini", "3nodes/node3.ini",
        "local/node1.ini", "local/node2.ini", "local/node3.ini",
        "single/node1.ini", "examples/split-node1.ini",
    };
    for (size_t f = 0; f < sizeof files / sizeof files[0]; f++) {
        char path[512];
        snprintf(path, sizeof path, "%s/conf/%s", KIVFS_SOURCE_DIR, files[f]);
        struct kivfs_config *c;
        int rc = kivfs_config_load(path, &c, g_err, sizeof g_err);
        CHECK(rc == KIVFS_OK);
        if (rc != KIVFS_OK) { fprintf(stderr, "     %s\n", g_err); continue; }
        for (int m = KIVFS_MOD_AUTH; m < KIVFS_MOD_COUNT; m++) {
            struct kivfs_modconf mc;
            /* klíče navíc jako v main.c modulů (Sync: order_cli, DB: backend) */
            static const char *const sync_keys[] = { "order_cli", NULL };
            static const char *const db_keys[]   = { "backend", NULL };
            const char *const *extra = m == KIVFS_MOD_SYNC ? sync_keys
                                     : m == KIVFS_MOD_DB   ? db_keys : NULL;
            rc = kivfs_modconf_from(c, (enum kivfs_module)m, extra, &mc, g_err, sizeof g_err);
            CHECK(rc == KIVFS_OK);
            if (rc != KIVFS_OK)
                fprintf(stderr, "     %s (%s): %s\n", files[f], kivfs_module_name(m), g_err);
            CHECK(mc.node_count == (strncmp(files[f], "single/", 7) == 0 ? 1u : 3u));
        }
        kivfs_config_free(c);
    }

    /* lokální šablona: node2 má porty 311xx, nodeN nesou porty */
    struct kivfs_config *c;
    struct kivfs_modconf mc;
    char path[512];
    snprintf(path, sizeof path, "%s/conf/local/node2.ini", KIVFS_SOURCE_DIR);
    if (kivfs_config_load(path, &c, g_err, sizeof g_err) == KIVFS_OK) {
        CHECK(kivfs_modconf_from(c, KIVFS_MOD_SYNC, NULL, &mc, g_err, sizeof g_err) == KIVFS_OK);
        CHECK(mc.local_node == 2 && mc.advertise.port == 31102);
        CHECK(mc.deps[KIVFS_MOD_VFS].port == 31103);
        CHECK(kivfs_modconf_node(&mc, 3)->sync.port == 31202);
        kivfs_config_free(c);
    }
}

/* [cluster] op_timeout_ms: výchozí, vlastní, mimo rozsah. */
static void test_op_timeout(void)
{
    struct kivfs_modconf mc;
    int rc = modconf_text(CLUSTER MODULES, KIVFS_MOD_AUTH, NULL, &mc);
    CHECK(rc == KIVFS_OK && mc.op_timeout_ms == KIVFS_OP_TIMEOUT_DEFAULT_MS);

    rc = modconf_text("[cluster]\nop_timeout_ms = 1500\nlocal = node1\nnode1 = a\n" MODULES,
                      KIVFS_MOD_SYNC, NULL, &mc);
    CHECK(rc == KIVFS_OK && mc.op_timeout_ms == 1500);
    if (rc != KIVFS_OK) fprintf(stderr, "     %s\n", g_err);

    rc = modconf_text("[cluster]\nop_timeout_ms = 10\nlocal = node1\nnode1 = a\n" MODULES,
                      KIVFS_MOD_SYNC, NULL, &mc);
    EXPECT_ERR(rc, ":2:", "op_timeout_ms");
    rc = modconf_text("[cluster]\nlocal = node1\nnode1 = a\nop_timeout_ms = x\n" MODULES,
                      KIVFS_MOD_SYNC, NULL, &mc);
    EXPECT_ERR(rc, ":4:", "op_timeout_ms");
}

int main(void)
{
    test_parser_ok();
    test_parser_errors();
    test_modconf_ok();
    test_modconf_errors();
    test_templates();
    test_op_timeout();
    printf("config: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
