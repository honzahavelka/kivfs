/*
 * Testy SHA-256 (util/sha256.h): vektory FIPS 180-4, hranice paddingu,
 * nezávislost na dělení dat do update, převod na hex a zpět.
 * Hranice 55–128 B ověřeny proti Python hashlib (data i*7+3).
 */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L        /* rand_r */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/errors.h"
#include "util/sha256.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

static int hash_is(const void *data, size_t n, const char *hex)
{
    struct kivfs_digest d;
    char got[KIVFS_DIGEST_HEX_LEN + 1];
    kivfs_sha256(data, n, &d);
    kivfs_digest_hex(&d, got);
    if (strcmp(got, hex) != 0) {
        fprintf(stderr, "  n=%zu: %s != %s\n", n, got, hex);
        return 0;
    }
    return 1;
}

static void test_fips_vectors(void)
{
    CHECK(hash_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(hash_is(NULL, 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(hash_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    {
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* 448 bitů */
        CHECK(hash_is(m, strlen(m), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }
    {
        const char *m = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"; /* 896 bitů */
        CHECK(hash_is(m, strlen(m), "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    }
    {
        /* milion 'a' – přes update po 1000 B */
        struct kivfs_sha256 c;
        struct kivfs_digest d;
        char a[1000], hex[KIVFS_DIGEST_HEX_LEN + 1];
        memset(a, 'a', sizeof a);
        kivfs_sha256_init(&c);
        for (int i = 0; i < 1000; i++)
            kivfs_sha256_update(&c, a, sizeof a);
        kivfs_sha256_final(&c, &d);
        kivfs_digest_hex(&d, hex);
        CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    }
}

static void test_padding_boundaries(void)
{
    static const struct { size_t n; const char *hex; } v[] = {
        {  55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b" },
        {  56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27" },
        {  57, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73" },
        {  63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055" },
        {  64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241" },
        {  65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e" },
        { 119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e" },
        { 120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5" },
        { 128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6" },
    };
    uint8_t buf[128];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 7 + 3);
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
        CHECK(hash_is(buf, v[i].n, v[i].hex));
}

/* Výsledek nesmí záviset na tom, jak se data rozdělí do update. */
static void test_split_independence(void)
{
    enum { N = 5000 };
    uint8_t *data = malloc(N);
    struct kivfs_digest whole, part;
    unsigned seed = 12345;

    CHECK(data != NULL);
    if (data == NULL)
        return;
    for (size_t i = 0; i < N; i++)
        data[i] = (uint8_t)(rand_r(&seed) >> 8);

    for (size_t len = 0; len <= N; len += (len < 200 ? 1 : 97)) {
        kivfs_sha256(data, len, &whole);
        for (int round = 0; round < 3; round++) {
            struct kivfs_sha256 c;
            size_t off = 0;
            kivfs_sha256_init(&c);
            while (off < len) {
                size_t k = (size_t)(rand_r(&seed) % 150);   /* i 0 B */
                if (k > len - off)
                    k = len - off;
                kivfs_sha256_update(&c, data + off, k);
                off += k;
            }
            kivfs_sha256_final(&c, &part);
            CHECK(kivfs_digest_eq(&whole, &part));
        }
    }
    free(data);
}

static void test_hex(void)
{
    struct kivfs_digest d, e;
    char hex[KIVFS_DIGEST_HEX_LEN + 1];

    kivfs_sha256("abc", 3, &d);
    kivfs_digest_hex(&d, hex);
    CHECK(strlen(hex) == KIVFS_DIGEST_HEX_LEN);
    CHECK(kivfs_digest_from_hex(hex, &e) == KIVFS_OK);
    CHECK(kivfs_digest_eq(&d, &e));
    CHECK(kivfs_digest_from_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", &e) == KIVFS_OK);
    CHECK(kivfs_digest_eq(&d, &e));

    memset(&e, 0x55, sizeof e);
    CHECK(kivfs_digest_from_hex("", &e) == KIVFS_EINVAL);
    CHECK(kivfs_digest_from_hex("ba78", &e) == KIVFS_EINVAL);
    hex[10] = 'g';
    CHECK(kivfs_digest_from_hex(hex, &e) == KIVFS_EINVAL);
    hex[10] = 'b';
    {
        char longer[KIVFS_DIGEST_HEX_LEN + 2];
        memcpy(longer, hex, KIVFS_DIGEST_HEX_LEN);
        longer[KIVFS_DIGEST_HEX_LEN] = '0';
        longer[KIVFS_DIGEST_HEX_LEN + 1] = '\0';
        CHECK(kivfs_digest_from_hex(longer, &e) == KIVFS_EINVAL);
    }
    CHECK(e.b[0] == 0x55);                     /* při chybě se out nemění */

    kivfs_sha256("abd", 3, &e);
    CHECK(!kivfs_digest_eq(&d, &e));
}

int main(void)
{
    test_fips_vectors();
    test_padding_boundaries();
    test_split_independence();
    test_hex();
    printf("sha256: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
