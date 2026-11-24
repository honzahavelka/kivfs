/*
 * Testy datových struktur operací FS (proto/fsproto.h) – bez sítě.
 */
#include <stdio.h>
#include <string.h>

#include "proto/errors.h"
#include "proto/fsproto.h"
#include "proto/packet.h"
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

static void test_structs(void)
{
    struct kivfs_packet p;
    struct kivfs_pkt_reader r;
    struct kivfs_attr a = { 0x0102030405060708ull, KIVFS_MODE_DIR | 0755, 1000, 100,
                            4096, 1732480000123ull, 1732480000456ull }, a2;
    struct kivfs_ctx c = { 1000, 1000 }, c2;
    uint32_t mode, uid, gid;
    uint64_t id;
    const char *name;
    uint32_t nlen;

    kivfs_pkt_init(&p, KIVFS_REQ_STAT, 1);
    CHECK(kivfs_attr_add(&p, &a) == KIVFS_OK);
    CHECK(kivfs_ctx_add(&p, &c) == KIVFS_OK);
    CHECK(kivfs_mknode_add(&p, KIVFS_MODE_DIR | 0700, 7, 8) == KIVFS_OK);
    CHECK(kivfs_entry_add(&p, 42, KIVFS_MODE_FILE | 0644, "soubor.txt", 10) == KIVFS_OK);
    CHECK(kivfs_entry_add(&p, 43, KIVFS_MODE_DIR, "", 0) == KIVFS_OK);
    /* úspora: attr 4 + 44, ctx 4 + 8, mknode 4 + 12, entry 4 + 12 + 10, entry 4 + 12 */
    CHECK(p.header.data_size == 48 + 12 + 16 + 26 + 16);

    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_attr_read(&r, &a2) == KIVFS_OK);
    CHECK(a2.id == a.id && a2.mode == a.mode && a2.uid == 1000 && a2.gid == 100
          && a2.size == 4096 && a2.mtime_ms == a.mtime_ms && a2.ctime_ms == a.ctime_ms);
    CHECK(kivfs_ctx_read(&r, &c2) == KIVFS_OK && c2.uid == 1000 && c2.gid == 1000);
    CHECK(kivfs_mknode_read(&r, &mode, &uid, &gid) == KIVFS_OK
          && mode == (KIVFS_MODE_DIR | 0700) && uid == 7 && gid == 8);
    CHECK(kivfs_entry_read(&r, &id, &mode, &name, &nlen) == KIVFS_OK);
    CHECK(id == 42 && mode == (KIVFS_MODE_FILE | 0644) && nlen == 10 && memcmp(name, "soubor.txt", 10) == 0);
    CHECK(kivfs_entry_read(&r, &id, &mode, &name, &nlen) == KIVFS_OK && id == 43 && nlen == 0);
    CHECK(kivfs_attr_read(&r, &a2) == KIVFS_EEND);
    kivfs_pkt_free(&p);

    char longname[KIVFS_NAME_MAX + 2];
    memset(longname, 'x', sizeof longname);
    kivfs_pkt_init(&p, KIVFS_REQ_LIST, 1);
    CHECK(kivfs_entry_add(&p, 1, 0, longname, KIVFS_NAME_MAX) == KIVFS_OK);
    CHECK(kivfs_entry_add(&p, 1, 0, longname, KIVFS_NAME_MAX + 1) == KIVFS_EINVAL);
    kivfs_pkt_free(&p);
}

/* Krátký záznam = MALFORMED a reader se neposune; delší (budoucí verze
 * s polem navíc) projde. */
static void test_short_and_long(void)
{
    struct kivfs_packet p;
    struct kivfs_pkt_reader r;
    struct kivfs_attr a;
    struct kivfs_ctx c;
    uint8_t buf[KIVFS_ATTR_WIRE_SIZE + 8];
    memset(buf, 0, sizeof buf);
    buf[7] = 5;                                  /* id = 5 */

    kivfs_pkt_init(&p, KIVFS_REQ_STAT, 1);
    kivfs_pkt_add_bytes(&p, buf, KIVFS_ATTR_WIRE_SIZE - 1);
    kivfs_pkt_add_bytes(&p, buf, sizeof buf);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_attr_read(&r, &a) == KIVFS_EMALFORMED);
    CHECK(r.pos == 0);
    CHECK(kivfs_ctx_read(&r, &c) == KIVFS_OK);   /* stejný záznam jako ctx (>= 8 B) */
    CHECK(kivfs_attr_read(&r, &a) == KIVFS_OK && a.id == 5);
    CHECK(kivfs_pkt_reader_at_end(&r));
    kivfs_pkt_free(&p);
}

static int path_ok(const char *s)
{
    struct kivfs_packet p;
    struct kivfs_pkt_reader r;
    char dst[KIVFS_PATH_MAX + 1];
    kivfs_pkt_init(&p, KIVFS_REQ_STAT, 1);
    kivfs_pkt_add_bytes(&p, s, (uint32_t)strlen(s));
    kivfs_pkt_reader_init(&r, &p);
    int rc = kivfs_path_read(&r, dst);
    int ok = (rc == KIVFS_OK && strcmp(dst, s) == 0);
    if (rc != KIVFS_OK)
        CHECK(r.pos == 0);
    kivfs_pkt_free(&p);
    return ok;
}

static void test_paths(void)
{
    CHECK(path_ok("/"));
    CHECK(path_ok("/a"));
    CHECK(path_ok("/a/b.c/d"));
    CHECK(path_ok("/...") && path_ok("/.a") && path_ok("/a..b"));
    CHECK(!path_ok(""));
    CHECK(!path_ok("a"));
    CHECK(!path_ok("a/b"));
    CHECK(!path_ok("/a/"));
    CHECK(!path_ok("//"));
    CHECK(!path_ok("/a//b"));
    CHECK(!path_ok("/."));
    CHECK(!path_ok("/a/./b"));
    CHECK(!path_ok("/a/.."));

    char big[KIVFS_PATH_MAX + 8];
    big[0] = '/';
    memset(big + 1, 'n', KIVFS_NAME_MAX);
    big[1 + KIVFS_NAME_MAX] = '\0';
    CHECK(path_ok(big));                         /* jméno přesně 255 */
    big[1 + KIVFS_NAME_MAX] = 'n';
    big[2 + KIVFS_NAME_MAX] = '\0';
    CHECK(!path_ok(big));                        /* 256 */

    /* cesta přesně 4096 (složky po 100 znacích) a 4097 */
    size_t len = 0;
    while (len + 101 <= KIVFS_PATH_MAX) {
        big[len] = '/';
        memset(big + len + 1, 'p', 100);
        len += 101;
    }
    big[len] = '/';
    memset(big + len + 1, 'q', KIVFS_PATH_MAX - len - 1);
    big[KIVFS_PATH_MAX] = '\0';
    CHECK(strlen(big) == KIVFS_PATH_MAX && path_ok(big));
    big[KIVFS_PATH_MAX] = 'q';
    big[KIVFS_PATH_MAX + 1] = '\0';
    CHECK(!path_ok(big));

    /* bajt 0 uprostřed */
    struct kivfs_packet p;
    struct kivfs_pkt_reader r;
    char dst[KIVFS_PATH_MAX + 1];
    kivfs_pkt_init(&p, KIVFS_REQ_STAT, 1);
    kivfs_pkt_add_bytes(&p, "/a\0b", 4);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_path_read(&r, dst) == KIVFS_EMALFORMED);
    kivfs_pkt_free(&p);
}

static void test_split(void)
{
    char parent[KIVFS_PATH_MAX + 1];
    const char *name;
    CHECK(kivfs_path_split("/a/b/c", parent, &name) == KIVFS_OK
          && strcmp(parent, "/a/b") == 0 && strcmp(name, "c") == 0);
    CHECK(kivfs_path_split("/a", parent, &name) == KIVFS_OK
          && strcmp(parent, "/") == 0 && strcmp(name, "a") == 0);
    CHECK(kivfs_path_split("/", parent, &name) == KIVFS_EINVAL);
}

int main(void)
{
    test_structs();
    test_short_and_long();
    test_paths();
    test_split();
    printf("fsproto: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
