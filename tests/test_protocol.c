/*
 * Unit testy pro errors.c, protocol.c a packet.c.
 * Bez externí knihovny: CHECK zaznamená selhání a pokračuje dál,
 * main na konci vrátí 1, pokud cokoli selhalo (to stačí pro ctest).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "protocol.h"
#include "packet.h"

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

/* Jako CHECK, ale při neshodě vypíše skutečný návratový kód. */
#define CHECK_RC(expr, expected)                                           \
    do {                                                                   \
        int rc_ = (expr);                                                  \
        g_checks++;                                                        \
        if (rc_ != (expected)) {                                           \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s -> %d (%s), expected %s\n",   \
                    __FILE__, __LINE__, #expr, rc_, kivfs_strerror(rc_),   \
                    #expected);                                            \
        }                                                                  \
    } while (0)

/* Paket z původního zadání: požadavek 27, ts 1337016539, záznam "/kivfs". */
static const uint8_t EXAMPLE_WIRE[30] = {
    0x00, 0x00, 0x7A, 0x69,                         /* magic 31337 */
    0x00, 0x00, 0x00, 0x00, 0x4F, 0xB1, 0x40, 0xDB, /* timestamp 1337016539 */
    0x00, 0x1B,                                     /* request_code 27 */
    0x00, 0x00,                                     /* return_value 0 */
    0x00, 0x00, 0x00, 0x0A,                         /* data_size 10 */
    0x00, 0x00, 0x00, 0x06,                         /* record_size 6 */
    '/', 'k', 'i', 'v', 'f', 's'
};

/* Pomocník: paket nad cizím bufferem (pro testy readeru na ručně
 * poskládaných, i vadných datech). Takový paket se NESMÍ uvolnit. */
static struct kivfs_packet raw_packet(uint8_t *data, uint32_t size)
{
    struct kivfs_packet p;
    kivfs_pkt_init(&p, 1, 0);
    p.data = data;
    p.capacity = size;
    p.header.data_size = size;
    return p;
}

/* ------------------------------------------------------------------ */

static void test_example_from_spec(void)
{
    struct kivfs_packet p;
    kivfs_pkt_init(&p, 27, 1337016539u);
    CHECK_RC(kivfs_pkt_add_string(&p, "/kivfs"), KIVFS_OK);
    CHECK(p.header.data_size == 10);

    uint8_t wire[30];
    kivfs_proto_header_encode(&p.header, wire);
    memcpy(wire + KIVFS_HEADER_WIRE_SIZE, p.data, p.header.data_size);
    CHECK(memcmp(wire, EXAMPLE_WIRE, sizeof wire) == 0);

    /* A zpět: dekódovat hlavičku a přečíst záznam. */
    struct kivfs_header h;
    CHECK_RC(kivfs_proto_header_decode(EXAMPLE_WIRE, &h), KIVFS_OK);
    CHECK(h.magic_number == KIVFS_MAGIC);
    CHECK(h.timestamp == 1337016539u);
    CHECK(h.request_code == 27);
    CHECK(h.return_value == 0);
    CHECK(h.data_size == 10);

    uint8_t payload[10];
    memcpy(payload, EXAMPLE_WIRE + KIVFS_HEADER_WIRE_SIZE, sizeof payload);
    struct kivfs_packet rx = raw_packet(payload, sizeof payload);
    struct kivfs_pkt_reader r;
    char path[16];
    kivfs_pkt_reader_init(&r, &rx);
    CHECK_RC(kivfs_pkt_read_string(&r, path, sizeof path), KIVFS_OK);
    CHECK(strcmp(path, "/kivfs") == 0);
    CHECK(kivfs_pkt_reader_at_end(&r));

    kivfs_pkt_free(&p);
}

static void test_header_roundtrip_and_validation(void)
{
    struct kivfs_header in = {
        .magic_number = KIVFS_MAGIC,
        .timestamp    = 0x0102030405060708ull,
        .request_code = 0x801B,
        .return_value = KIVFS_STATUS_INTERNAL,   /* záporné číslo */
        .data_size    = KIVFS_MAX_PAYLOAD,       /* hranice: ještě OK */
    };
    struct kivfs_header out;
    uint8_t wire[KIVFS_HEADER_WIRE_SIZE];

    kivfs_proto_header_encode(&in, wire);
    CHECK(wire[4] == 0x01 && wire[11] == 0x08);       /* big-endian timestamp */
    CHECK(wire[14] == 0xFF && wire[15] == 0xF9);      /* -7 jako 0xFFF9 */
    CHECK_RC(kivfs_proto_header_decode(wire, &out), KIVFS_OK);
    CHECK(out.timestamp == in.timestamp);
    CHECK(out.request_code == in.request_code);
    CHECK(out.return_value == KIVFS_STATUS_INTERNAL);
    CHECK(out.data_size == in.data_size);

    in.return_value = INT16_MIN;                       /* krajní hodnota */
    kivfs_proto_header_encode(&in, wire);
    CHECK_RC(kivfs_proto_header_decode(wire, &out), KIVFS_OK);
    CHECK(out.return_value == INT16_MIN);

    in.data_size = KIVFS_MAX_PAYLOAD + 1;              /* o 1 přes */
    kivfs_proto_header_encode(&in, wire);
    CHECK_RC(kivfs_proto_header_decode(wire, &out), KIVFS_ETOOBIG);

    in.data_size = 0;
    in.magic_number = 0xDEADBEEF;
    kivfs_proto_header_encode(&in, wire);
    CHECK_RC(kivfs_proto_header_decode(wire, &out), KIVFS_EBADMAGIC);
}

static void test_response_code_helpers(void)
{
    CHECK(!kivfs_proto_is_response(27));
    CHECK(kivfs_proto_response_code(27) == 0x801B);
    CHECK(kivfs_proto_is_response(0x801B));
    CHECK(kivfs_proto_base_code(0x801B) == 27);
    CHECK(kivfs_proto_base_code(27) == 27);
}

static void test_reader_typed_roundtrip(void)
{
    struct kivfs_packet p;
    kivfs_pkt_init(&p, 5, 1);
    CHECK_RC(kivfs_pkt_add_string(&p, "abc"), KIVFS_OK);
    CHECK_RC(kivfs_pkt_add_u32(&p, 0xCAFEBABEu), KIVFS_OK);
    CHECK_RC(kivfs_pkt_add_u64(&p, 0x1122334455667788ull), KIVFS_OK);
    CHECK_RC(kivfs_pkt_add_bytes(&p, NULL, 0), KIVFS_OK);   /* prázdný záznam */
    CHECK_RC(kivfs_pkt_add_string(&p, ""), KIVFS_OK);
    CHECK(p.header.data_size == (4 + 3) + (4 + 4) + (4 + 8) + 4 + 4);

    struct kivfs_pkt_reader r;
    char s[8];
    uint32_t u32;
    uint64_t u64;
    const uint8_t *ptr;
    uint32_t len;

    kivfs_pkt_reader_init(&r, &p);
    CHECK_RC(kivfs_pkt_read_string(&r, s, sizeof s), KIVFS_OK);
    CHECK(strcmp(s, "abc") == 0);
    CHECK_RC(kivfs_pkt_read_u32(&r, &u32), KIVFS_OK);
    CHECK(u32 == 0xCAFEBABEu);
    CHECK_RC(kivfs_pkt_read_u64(&r, &u64), KIVFS_OK);
    CHECK(u64 == 0x1122334455667788ull);
    CHECK_RC(kivfs_pkt_read_bytes(&r, &ptr, &len), KIVFS_OK);
    CHECK(len == 0);
    CHECK_RC(kivfs_pkt_read_string(&r, s, sizeof s), KIVFS_OK);
    CHECK(s[0] == '\0');
    CHECK(kivfs_pkt_reader_at_end(&r));
    CHECK_RC(kivfs_pkt_read_u32(&r, &u32), KIVFS_EEND);

    kivfs_pkt_free(&p);
    kivfs_pkt_free(&p);                     /* idempotentní */
    CHECK(p.data == NULL && p.header.data_size == 0);
}

static void test_reader_type_errors_do_not_advance(void)
{
    struct kivfs_packet p;
    kivfs_pkt_init(&p, 5, 1);
    CHECK_RC(kivfs_pkt_add_u64(&p, 42), KIVFS_OK);
    CHECK_RC(kivfs_pkt_add_string(&p, "hello"), KIVFS_OK);

    struct kivfs_pkt_reader r;
    uint32_t u32;
    uint64_t u64;
    char small[5];        /* "hello" potřebuje 6 (s '\0') */
    char exact[6];

    kivfs_pkt_reader_init(&r, &p);
    CHECK_RC(kivfs_pkt_read_u32(&r, &u32), KIVFS_ETYPE);  /* záznam má 8 B */
    CHECK(r.pos == 0);                                     /* neposunulo se */
    CHECK_RC(kivfs_pkt_read_u64(&r, &u64), KIVFS_OK);
    CHECK(u64 == 42);

    CHECK_RC(kivfs_pkt_read_string(&r, small, sizeof small), KIVFS_ETYPE);
    CHECK_RC(kivfs_pkt_read_string(&r, small, 0), KIVFS_ETYPE);
    CHECK_RC(kivfs_pkt_read_string(&r, exact, sizeof exact), KIVFS_OK);
    CHECK(strcmp(exact, "hello") == 0);
    CHECK(kivfs_pkt_reader_at_end(&r));

    kivfs_pkt_free(&p);
}

static void test_reader_malformed_input(void)
{
    struct kivfs_pkt_reader r;
    const uint8_t *ptr;
    uint32_t len;

    /* Délka záznamu (5) přesahuje zbytek dat (jen 3 B). */
    uint8_t overflow[] = { 0, 0, 0, 5, 'a', 'b', 'c' };
    struct kivfs_packet p1 = raw_packet(overflow, sizeof overflow);
    kivfs_pkt_reader_init(&r, &p1);
    CHECK_RC(kivfs_pkt_read_bytes(&r, &ptr, &len), KIVFS_EMALFORMED);
    CHECK(r.pos == 0);

    /* Obří délka 0xFFFFFFFF – kontrola nesmí přetéct a "projít". */
    uint8_t huge[] = { 0xFF, 0xFF, 0xFF, 0xFF, 'x' };
    struct kivfs_packet p2 = raw_packet(huge, sizeof huge);
    kivfs_pkt_reader_init(&r, &p2);
    CHECK_RC(kivfs_pkt_read_bytes(&r, &ptr, &len), KIVFS_EMALFORMED);

    /* Na konci zbyly jen 2 bajty – useknutá délka dalšího záznamu. */
    uint8_t trunc[] = { 0, 0, 0, 1, 'z', 0, 0 };
    struct kivfs_packet p3 = raw_packet(trunc, sizeof trunc);
    kivfs_pkt_reader_init(&r, &p3);
    CHECK_RC(kivfs_pkt_read_bytes(&r, &ptr, &len), KIVFS_OK);
    CHECK(len == 1 && ptr[0] == 'z');
    CHECK(!kivfs_pkt_reader_at_end(&r));
    CHECK_RC(kivfs_pkt_read_bytes(&r, &ptr, &len), KIVFS_EMALFORMED);

    /* Nekonzistentní paket: data == NULL, data_size > 0 -> chová se jako prázdný. */
    struct kivfs_packet p4;
    kivfs_pkt_init(&p4, 1, 0);
    p4.header.data_size = 100;
    kivfs_pkt_reader_init(&r, &p4);
    CHECK_RC(kivfs_pkt_read_bytes(&r, &ptr, &len), KIVFS_EEND);
}

static void test_response_and_ref(void)
{
    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, 27, 1000);
    CHECK_RC(kivfs_pkt_add_string(&req, "/kivfs"), KIVFS_OK);

    CHECK_RC(kivfs_pkt_init_response(&resp, &req, 1005, KIVFS_STATUS_NOT_FOUND), KIVFS_OK);
    CHECK(kivfs_proto_is_response(resp.header.request_code));
    CHECK(kivfs_proto_base_code(resp.header.request_code) == 27);
    CHECK(resp.header.timestamp == 1005);
    CHECK(resp.header.return_value == KIVFS_STATUS_NOT_FOUND);
    CHECK(resp.header.data_size == 4 + 8);              /* jen ref */

    uint64_t ref = 0;
    CHECK_RC(kivfs_pkt_response_ref(&resp, &ref), KIVFS_OK);
    CHECK(ref == 1000);

    /* Data odpovědi až za refem. */
    CHECK_RC(kivfs_pkt_add_u32(&resp, 7), KIVFS_OK);
    struct kivfs_pkt_reader r;
    uint64_t r_ref;
    uint32_t val;
    kivfs_pkt_reader_init(&r, &resp);
    CHECK_RC(kivfs_pkt_read_u64(&r, &r_ref), KIVFS_OK);
    CHECK(r_ref == 1000);
    CHECK_RC(kivfs_pkt_read_u32(&r, &val), KIVFS_OK);
    CHECK(val == 7);
    CHECK(kivfs_pkt_reader_at_end(&r));

    /* Požadavek není odpověď -> ref nelze přečíst. */
    CHECK_RC(kivfs_pkt_response_ref(&req, &ref), KIVFS_EMALFORMED);

    /* Odpověď bez refu (prázdná) -> EMALFORMED. */
    struct kivfs_packet bad;
    kivfs_pkt_init(&bad, kivfs_proto_response_code(27), 1);
    CHECK_RC(kivfs_pkt_response_ref(&bad, &ref), KIVFS_EMALFORMED);

    kivfs_pkt_free(&req);
    kivfs_pkt_free(&resp);
}

static void test_payload_limit(void)
{
    struct kivfs_packet p;
    kivfs_pkt_init(&p, 5, 1);

    /* Záznam o délce MAX by s 4B délkou přesáhl limit -> nic se nealokuje. */
    CHECK_RC(kivfs_pkt_add_bytes(&p, NULL, KIVFS_MAX_PAYLOAD), KIVFS_ETOOBIG);
    CHECK(p.data == NULL && p.header.data_size == 0);

    /* Záznam, který limit přesně vyplní. */
    uint8_t *big = calloc(1, KIVFS_MAX_PAYLOAD);
    CHECK(big != NULL);
    if (big != NULL) {
        CHECK_RC(kivfs_pkt_add_bytes(&p, big, KIVFS_MAX_PAYLOAD - 4), KIVFS_OK);
        CHECK(p.header.data_size == KIVFS_MAX_PAYLOAD);

        /* Ani prázdný záznam (4 B) se už nevejde; paket zůstane beze změny. */
        uint32_t before = p.header.data_size;
        CHECK_RC(kivfs_pkt_add_bytes(&p, NULL, 0), KIVFS_ETOOBIG);
        CHECK_RC(kivfs_pkt_add_u32(&p, 1), KIVFS_ETOOBIG);
        CHECK(p.header.data_size == before);
        free(big);
    }
    kivfs_pkt_free(&p);
}

static void test_strerror(void)
{
    CHECK(strcmp(kivfs_strerror(KIVFS_OK), "success") == 0);
    CHECK(strcmp(kivfs_strerror(KIVFS_EUNEXPECTED), "unknown error") != 0);
    CHECK(strcmp(kivfs_strerror(-999), "unknown error") == 0);
}

static void test_status_str(void)
{
    CHECK(strcmp(kivfs_status_str(KIVFS_STATUS_OK), "OK") == 0);
    CHECK(strcmp(kivfs_status_str(KIVFS_STATUS_BUSY), "BUSY") == 0);
    CHECK(strcmp(kivfs_status_str(KIVFS_STATUS_INTERNAL), "INTERNAL") == 0);
    /* hodnota od protistrany může být cokoli */
    CHECK(strcmp(kivfs_status_str(1234), "UNKNOWN_STATUS") == 0);
    CHECK(strcmp(kivfs_status_str(-32768), "UNKNOWN_STATUS") == 0);
}

int main(void)
{
    test_example_from_spec();
    test_header_roundtrip_and_validation();
    test_response_code_helpers();
    test_reader_typed_roundtrip();
    test_reader_type_errors_do_not_advance();
    test_reader_malformed_input();
    test_response_and_ref();
    test_payload_limit();
    test_strerror();
    test_status_str();

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
