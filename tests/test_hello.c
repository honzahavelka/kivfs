/*
 * Testy HELLO a diagnostických bloků STATUS (hello.h, diag.h) – bez sítě.
 */
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "errors.h"
#include "hello.h"
#include "packet.h"
#include "protocol.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

static void test_roundtrip(void)
{
    struct kivfs_hello a = { 3, KIVFS_MOD_SYNC, KIVFS_PROTO_VERSION, 0x1122334455667788ull, 0xCAFE };
    struct kivfs_hello b = { 2, KIVFS_MOD_VFS, KIVFS_PROTO_VERSION, 42, 0xCAFE };
    struct kivfs_hello x;
    struct kivfs_packet req, resp;

    CHECK(kivfs_hello_request(&req, 77, &a) == KIVFS_OK);
    CHECK(req.header.request_code == KIVFS_REQ_HELLO && req.header.timestamp == 77);
    CHECK(kivfs_hello_parse_request(&req, &x) == KIVFS_OK);
    CHECK(x.node_id == a.node_id && x.module == a.module && x.version == a.version
          && x.incarnation == a.incarnation && x.cluster_hash == a.cluster_hash);

    CHECK(kivfs_hello_response(&resp, &req, 5, KIVFS_STATUS_REJECTED, &b) == KIVFS_OK);
    CHECK(resp.header.return_value == KIVFS_STATUS_REJECTED);
    uint64_t ref;
    CHECK(kivfs_pkt_response_ref(&resp, &ref) == KIVFS_OK && ref == 77);
    CHECK(kivfs_hello_parse_response(&resp, &x) == KIVFS_OK);
    CHECK(x.node_id == 2 && x.module == KIVFS_MOD_VFS && x.incarnation == 42);
    /* odpověď není požadavek: první záznam je u64 ref, ne u32 */
    CHECK(kivfs_hello_parse_request(&resp, &x) == KIVFS_EMALFORMED);
    kivfs_pkt_free(&req);
    kivfs_pkt_free(&resp);

    /* starý formát (jen node_id) a přebytečné záznamy = MALFORMED */
    kivfs_pkt_init(&req, KIVFS_REQ_HELLO, 1);
    kivfs_pkt_add_u32(&req, 0);
    CHECK(kivfs_hello_parse_request(&req, &x) == KIVFS_EMALFORMED);
    kivfs_pkt_free(&req);
    kivfs_hello_request(&req, 1, &a);
    kivfs_pkt_add_u32(&req, 9);
    CHECK(kivfs_hello_parse_request(&req, &x) == KIVFS_EMALFORMED);
    kivfs_pkt_free(&req);
}

static void test_may_call(void)
{
    int expect[KIVFS_MOD_COUNT][KIVFS_MOD_COUNT];
    memset(expect, 0, sizeof expect);
    expect[KIVFS_MOD_CLIENT][KIVFS_MOD_AUTH] = 1;
    expect[KIVFS_MOD_AUTH][KIVFS_MOD_SYNC] = 1;
    expect[KIVFS_MOD_SYNC][KIVFS_MOD_VFS] = 1;
    expect[KIVFS_MOD_SYNC][KIVFS_MOD_SYNC] = 1;
    expect[KIVFS_MOD_VFS][KIVFS_MOD_DB] = 1;
    expect[KIVFS_MOD_VFS][KIVFS_MOD_FS] = 1;
    for (int a = 0; a < KIVFS_MOD_COUNT; a++)
        for (int b = 0; b < KIVFS_MOD_COUNT; b++)
            CHECK(kivfs_module_may_call((uint32_t)a, (uint32_t)b) == expect[a][b]);
    CHECK(!kivfs_module_may_call(99, KIVFS_MOD_AUTH));
}

static void test_hash_and_incarnation(void)
{
    uint32_t a[] = { 1, 2, 3 }, b[] = { 1, 2, 4 }, c[] = { 1, 2 };
    CHECK(kivfs_cluster_hash(a, 3) == kivfs_cluster_hash(a, 3));
    CHECK(kivfs_cluster_hash(a, 3) != kivfs_cluster_hash(b, 3));
    CHECK(kivfs_cluster_hash(a, 3) != kivfs_cluster_hash(c, 2));
    CHECK(kivfs_cluster_hash(a, 3) != 0 && kivfs_cluster_hash(NULL, 0) != 0);
    uint64_t i1 = kivfs_incarnation_new(), i2 = kivfs_incarnation_new();
    CHECK(i1 != 0 && i2 != 0 && i1 != i2);
}

static void test_diag(void)
{
    struct kivfs_diag_module m1, m2, r1;
    memset(&m1, 0, sizeof m1);
    memset(&m2, 0, sizeof m2);
    m1.node = 1; m1.module = KIVFS_MOD_SYNC; m1.incarnation = 7; m1.ready = 1;
    m1.n_out = 2;
    m1.out[0] = (struct kivfs_diag_link){ KIVFS_MOD_VFS, 1, KIVFS_LINK_UP, 8 };
    m1.out[1] = (struct kivfs_diag_link){ KIVFS_MOD_SYNC, 2, KIVFS_LINK_CONNECTING, 0 };
    m1.n_in = 1;
    m1.in[0] = (struct kivfs_diag_peer){ KIVFS_MOD_AUTH, 1 };
    m2.node = 1; m2.module = KIVFS_MOD_DB; m2.incarnation = 9;

    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, KIVFS_REQ_STATUS, 3);
    CHECK(kivfs_pkt_init_response(&resp, &req, 4, KIVFS_STATUS_OK) == KIVFS_OK);
    CHECK(kivfs_diag_add(&resp, &m1) == KIVFS_OK);
    CHECK(kivfs_diag_add(&resp, &m2) == KIVFS_OK);

    struct kivfs_pkt_reader r;
    uint64_t ref;
    kivfs_pkt_reader_init(&r, &resp);
    kivfs_pkt_read_u64(&r, &ref);
    CHECK(kivfs_diag_read(&r, &r1) == KIVFS_OK);
    CHECK(r1.node == 1 && r1.module == KIVFS_MOD_SYNC && r1.ready == 1 && r1.n_out == 2);
    CHECK(r1.out[1].state == KIVFS_LINK_CONNECTING && r1.out[0].peer_incarnation == 8);
    CHECK(r1.n_in == 1 && r1.in[0].module == KIVFS_MOD_AUTH);
    CHECK(kivfs_diag_read(&r, &r1) == KIVFS_OK && r1.module == KIVFS_MOD_DB && r1.n_out == 0);
    CHECK(kivfs_diag_read(&r, &r1) == KIVFS_EEND);
    kivfs_pkt_free(&resp);

    /* n_out nad maximum = MALFORMED; useknutý blok = MALFORMED */
    kivfs_pkt_init_response(&resp, &req, 4, KIVFS_STATUS_OK);
    kivfs_pkt_add_u32(&resp, 1);
    kivfs_pkt_add_u32(&resp, 2);
    kivfs_pkt_add_u64(&resp, 3);
    kivfs_pkt_add_u32(&resp, 1);
    kivfs_pkt_add_u32(&resp, KIVFS_DIAG_MAX_LINKS + 1);
    kivfs_pkt_reader_init(&r, &resp);
    kivfs_pkt_read_u64(&r, &ref);
    CHECK(kivfs_diag_read(&r, &r1) == KIVFS_EMALFORMED);
    kivfs_pkt_free(&resp);
    kivfs_pkt_init_response(&resp, &req, 4, KIVFS_STATUS_OK);
    kivfs_pkt_add_u32(&resp, 1);
    kivfs_pkt_reader_init(&r, &resp);
    kivfs_pkt_read_u64(&r, &ref);
    CHECK(kivfs_diag_read(&r, &r1) == KIVFS_EMALFORMED);
    kivfs_pkt_free(&resp);
    kivfs_pkt_free(&req);
    CHECK(strcmp(kivfs_link_state_str(KIVFS_LINK_UP), "UP") == 0);
}

int main(void)
{
    test_roundtrip();
    test_may_call();
    test_hash_and_incarnation();
    test_diag();
    printf("hello: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
