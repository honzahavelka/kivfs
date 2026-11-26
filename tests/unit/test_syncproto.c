/*
 * Testy zpráv synchronizace (proto/syncproto.h) – bez sítě.
 */
#include <stdio.h>
#include <string.h>

#include "proto/errors.h"
#include "proto/fsproto.h"
#include "proto/packet.h"
#include "proto/protocol.h"
#include "proto/syncproto.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

static const struct kivfs_op_id OP = { 3, 0x1122334455667788ull, 0x0000000100000002ull };

static int op_eq(const struct kivfs_op_id *a, const struct kivfs_op_id *b)
{
    return kivfs_op_id_eq(a, b);
}

/* Všechny struktury tam a zpět + velikosti na drátě. */
static void test_roundtrip(void)
{
    struct kivfs_packet p;
    struct kivfs_pkt_reader r;
    struct kivfs_sync_hdr h = { 1, KIVFS_LANE_DB, OP, 0xA0B0C0D0E0F00102ull }, h2;
    struct kivfs_dbop d = { KIVFS_REQ_DB_MKNODE, 2, 1732480000123ull }, d2;
    struct kivfs_arch_hdr a = { 42, 77, OP }, a2;
    struct kivfs_sync_state s = { KIVFS_OPST_COMMITTED, 99 }, s2;
    struct kivfs_sync_clock c = { 5, 6, 41, 3 }, c2;

    kivfs_pkt_init(&p, KIVFS_REQ_SYNC_PROPOSE, 1);
    CHECK(kivfs_sync_hdr_add(&p, &h) == KIVFS_OK);
    CHECK(kivfs_dbop_add(&p, &d) == KIVFS_OK);
    CHECK(kivfs_arch_hdr_add(&p, &a) == KIVFS_OK);
    CHECK(kivfs_sync_state_add(&p, &s) == KIVFS_OK);
    CHECK(kivfs_sync_clock_add(&p, &c) == KIVFS_OK);
    CHECK(p.header.data_size == (4 + 36) + (4 + 16) + (4 + 36) + (4 + 12) + (4 + 32));

    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_sync_hdr_read(&r, &h2) == KIVFS_OK);
    CHECK(h2.epoch == 1 && h2.lane == KIVFS_LANE_DB && op_eq(&h2.op, &OP) && h2.t == h.t);
    CHECK(kivfs_dbop_read(&r, &d2) == KIVFS_OK);
    CHECK(d2.code == KIVFS_REQ_DB_MKNODE && d2.n_records == 2 && d2.time_ms == d.time_ms);
    CHECK(kivfs_arch_hdr_read(&r, &a2) == KIVFS_OK);
    CHECK(a2.index == 42 && a2.t == 77 && op_eq(&a2.op, &OP));
    CHECK(kivfs_sync_state_read(&r, &s2) == KIVFS_OK && s2.state == KIVFS_OPST_COMMITTED && s2.t == 99);
    CHECK(kivfs_sync_clock_read(&r, &c2) == KIVFS_OK && c2.tcli == 5 && c2.tdb == 6 && c2.last_index == 41
          && c2.db_pending == 3);
    CHECK(kivfs_sync_clock_read(&r, &c2) == KIVFS_EEND);
    CHECK(kivfs_pkt_reader_at_end(&r));
    kivfs_pkt_free(&p);
}

/* Rozložení sync_hdr na drátě (big-endian, offsety podle syncproto.h). */
static void test_wire_layout(void)
{
    static const uint8_t expect[36] = {
        0x00, 0x00, 0x00, 0x01,                          /* epoch */
        0x00, 0x00, 0x00, 0x01,                          /* lane = DB */
        0x00, 0x00, 0x00, 0x03,                          /* op.node */
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,  /* op.incarnation */
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,  /* op.seq */
        0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x01, 0x02   /* t */
    };
    struct kivfs_packet p;
    struct kivfs_pkt_reader r;
    struct kivfs_sync_hdr h = { 1, KIVFS_LANE_DB, OP, 0xA0B0C0D0E0F00102ull };
    const uint8_t *b;
    uint32_t len;

    kivfs_pkt_init(&p, KIVFS_REQ_SYNC_COMMIT, 1);
    CHECK(kivfs_sync_hdr_add(&p, &h) == KIVFS_OK);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_pkt_read_bytes(&r, &b, &len) == KIVFS_OK);
    CHECK(len == sizeof expect && memcmp(b, expect, sizeof expect) == 0);
    kivfs_pkt_free(&p);
}

/* Krátký záznam a neplatný pruh -> MALFORMED a reader se neposune;
 * delší záznam (novější verze) se přijme a přebytek ignoruje. */
static void test_short_long_invalid(void)
{
    struct kivfs_packet p;
    struct kivfs_pkt_reader r;
    struct kivfs_sync_hdr h;
    struct kivfs_dbop d;
    uint8_t buf[64];
    uint32_t pos;

    memset(buf, 0, sizeof buf);
    kivfs_pkt_init(&p, KIVFS_REQ_SYNC_PROPOSE, 1);
    CHECK(kivfs_pkt_add_bytes(&p, buf, KIVFS_SYNC_HDR_WIRE_SIZE - 1) == KIVFS_OK);
    kivfs_pkt_reader_init(&r, &p);
    pos = r.pos;
    CHECK(kivfs_sync_hdr_read(&r, &h) == KIVFS_EMALFORMED && r.pos == pos);
    kivfs_pkt_free(&p);

    /* lane = 2 (neexistuje) */
    buf[7] = 2;
    kivfs_pkt_init(&p, KIVFS_REQ_SYNC_PROPOSE, 1);
    CHECK(kivfs_pkt_add_bytes(&p, buf, KIVFS_SYNC_HDR_WIRE_SIZE) == KIVFS_OK);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_sync_hdr_read(&r, &h) == KIVFS_EMALFORMED && r.pos == pos);
    kivfs_pkt_free(&p);

    /* dbop o 8 B delší: přijmout, přebytek ignorovat */
    memset(buf, 0, sizeof buf);
    buf[3] = 101; buf[7] = 0; buf[15] = 9;
    kivfs_pkt_init(&p, KIVFS_REQ_SYNC_SUBMIT, 1);
    CHECK(kivfs_pkt_add_bytes(&p, buf, KIVFS_DBOP_WIRE_SIZE + 8) == KIVFS_OK);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_dbop_read(&r, &d) == KIVFS_OK && d.code == 101 && d.n_records == 0 && d.time_ms == 9);
    CHECK(kivfs_pkt_reader_at_end(&r));
    kivfs_pkt_free(&p);
}

/* Pořadí klíčů: t, pak node, incarnation, seq. */
static void test_key_cmp(void)
{
    struct kivfs_op_id a = { 1, 5, 5 }, b;

    CHECK(kivfs_sync_key_cmp(10, &a, 10, &a) == 0);
    b = a;                         CHECK(kivfs_sync_key_cmp(9, &b, 10, &a) < 0);
    b = a; b.node = 2;             CHECK(kivfs_sync_key_cmp(10, &a, 10, &b) < 0);
    b = a; b.node = 0; b.seq = 99; CHECK(kivfs_sync_key_cmp(10, &a, 10, &b) > 0);
    b = a; b.incarnation = 6; b.seq = 0;
    CHECK(kivfs_sync_key_cmp(10, &a, 10, &b) < 0);
    b = a; b.seq = 6;              CHECK(kivfs_sync_key_cmp(10, &a, 10, &b) < 0);
    /* t má přednost před vším ostatním */
    b = a; b.node = 0;             CHECK(kivfs_sync_key_cmp(11, &a, 10, &b) > 0);
    /* velká čísla (u64 se nesmí porovnávat odečtením) */
    CHECK(kivfs_sync_key_cmp(UINT64_MAX, &a, 0, &a) > 0);
    b = a;                         CHECK(kivfs_op_id_eq(&a, &b));
    b.seq++;                       CHECK(!kivfs_op_id_eq(&a, &b));
}

/* Paket PROPOSE pruhu DB: [sync_hdr][dbop n=2][path][mknode] + navíc [u32]. */
static void build_propose(struct kivfs_packet *p, uint32_t n_records)
{
    struct kivfs_sync_hdr h = { 1, KIVFS_LANE_DB, OP, 10 };
    struct kivfs_dbop d = { KIVFS_REQ_DB_MKNODE, n_records, 1234 };
    kivfs_pkt_init(p, KIVFS_REQ_SYNC_PROPOSE, 1);
    CHECK(kivfs_sync_hdr_add(p, &h) == KIVFS_OK);
    CHECK(kivfs_dbop_add(p, &d) == KIVFS_OK);
    CHECK(kivfs_pkt_add_string(p, "/a/b") == KIVFS_OK);
    CHECK(kivfs_mknode_add(p, KIVFS_MODE_DIR | 0755, 1000, 1000) == KIVFS_OK);
    CHECK(kivfs_pkt_add_u32(p, 0xCAFE) == KIVFS_OK);
}

static void test_dbop_skip_copy(void)
{
    struct kivfs_packet p, q;
    struct kivfs_pkt_reader r, s;
    struct kivfs_sync_hdr h;
    struct kivfs_dbop d;
    const uint8_t *start;
    uint32_t len, tail, pos, mode, uid, gid;
    char path[KIVFS_PATH_MAX + 1];

    /* skip: vrátí přesně bajty operace a reader stojí za ní */
    build_propose(&p, 2);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_sync_hdr_read(&r, &h) == KIVFS_OK);
    CHECK(kivfs_dbop_skip(&r, &start, &len) == KIVFS_OK);
    CHECK(len == (4 + 16) + (4 + 4) + (4 + 12));
    CHECK(start == p.data + 4 + KIVFS_SYNC_HDR_WIRE_SIZE);
    CHECK(kivfs_pkt_read_u32(&r, &tail) == KIVFS_OK && tail == 0xCAFE);

    /* copy z uložené kopie (reader nad holými bajty) do nového paketu */
    uint8_t saved[64];
    CHECK(len <= sizeof saved);
    memcpy(saved, start, len);
    struct kivfs_pkt_reader sr = { saved, len, 0 };
    struct kivfs_arch_hdr a = { 1, 10, OP }, a2;
    kivfs_pkt_init(&q, KIVFS_REQ_DB_APPLY, 2);
    CHECK(kivfs_arch_hdr_add(&q, &a) == KIVFS_OK);
    CHECK(kivfs_dbop_copy(&q, &sr) == KIVFS_OK);
    CHECK(kivfs_pkt_reader_at_end(&sr));
    CHECK(q.header.data_size == (4 + 36) + len);

    kivfs_pkt_reader_init(&s, &q);
    CHECK(kivfs_arch_hdr_read(&s, &a2) == KIVFS_OK && a2.index == 1 && op_eq(&a2.op, &OP));
    CHECK(kivfs_dbop_read(&s, &d) == KIVFS_OK && d.code == KIVFS_REQ_DB_MKNODE
          && d.n_records == 2 && d.time_ms == 1234);
    CHECK(kivfs_path_read(&s, path) == KIVFS_OK && strcmp(path, "/a/b") == 0);
    CHECK(kivfs_mknode_read(&s, &mode, &uid, &gid) == KIVFS_OK
          && mode == (KIVFS_MODE_DIR | 0755) && uid == 1000 && gid == 1000);
    CHECK(kivfs_pkt_reader_at_end(&s));
    kivfs_pkt_free(&q);
    kivfs_pkt_free(&p);

    /* n_records větší, než kolik záznamů zbývá -> MALFORMED, reader ani dst
     * se nezmění */
    build_propose(&p, 100);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_sync_hdr_read(&r, &h) == KIVFS_OK);
    pos = r.pos;
    CHECK(kivfs_dbop_skip(&r, &start, &len) == KIVFS_EMALFORMED && r.pos == pos);
    kivfs_pkt_init(&q, KIVFS_REQ_DB_APPLY, 2);
    CHECK(kivfs_dbop_copy(&q, &r) == KIVFS_EMALFORMED && r.pos == pos);
    CHECK(q.header.data_size == 0);
    kivfs_pkt_free(&q);
    kivfs_pkt_free(&p);

    /* operace bez záznamů (n = 0) */
    kivfs_pkt_init(&p, KIVFS_REQ_SYNC_SUBMIT, 1);
    d = (struct kivfs_dbop){ KIVFS_REQ_DB_RMNODE, 0, 1 };
    CHECK(kivfs_dbop_add(&p, &d) == KIVFS_OK);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_dbop_skip(&r, &start, &len) == KIVFS_OK && len == 4 + 16);
    CHECK(kivfs_pkt_reader_at_end(&r));
    kivfs_pkt_free(&p);

    /* prázdný payload */
    kivfs_pkt_init(&p, KIVFS_REQ_SYNC_SUBMIT, 1);
    kivfs_pkt_reader_init(&r, &p);
    CHECK(kivfs_dbop_skip(&r, &start, &len) == KIVFS_EEND);
    kivfs_pkt_free(&p);
}

static void test_status_names(void)
{
    CHECK(strcmp(kivfs_status_str(KIVFS_STATUS_TIMEOUT), "TIMEOUT") == 0);
    CHECK(strcmp(kivfs_status_str(KIVFS_STATUS_VIEW_MISMATCH), "VIEW_MISMATCH") == 0);
}

int main(void)
{
    test_roundtrip();
    test_wire_layout();
    test_short_long_invalid();
    test_key_cmp();
    test_dbop_skip_copy();
    test_status_names();
    printf("syncproto: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
