/*
 * KIVFS – SHA-256 podle FIPS 180-4 (viz sha256.h).
 */
#include "util/sha256.h"

#include <string.h>

#include "proto/errors.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* Zpracuje jeden 64B blok. Slova se skládají z bajtů ručně (big-endian),
 * takže výsledek nezávisí na pořadí bajtů stroje. */
static void compress(uint32_t h[8], const uint8_t blk[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16
             | (uint32_t)blk[4 * i + 2] << 8 | (uint32_t)blk[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = k + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void kivfs_sha256_init(struct kivfs_sha256 *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->buf_len = 0;
}

void kivfs_sha256_update(struct kivfs_sha256 *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    if (n == 0)
        return;
    c->total += n;

    if (c->buf_len > 0) {                       /* dorovnat rozpracovaný blok */
        size_t take = 64 - c->buf_len;
        if (take > n)
            take = n;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += (uint32_t)take;
        p += take;
        n -= take;
        if (c->buf_len < 64)
            return;
        compress(c->h, c->buf);
        c->buf_len = 0;
    }
    while (n >= 64) {                           /* celé bloky bez kopie */
        compress(c->h, p);
        p += 64;
        n -= 64;
    }
    if (n > 0) {
        memcpy(c->buf, p, n);
        c->buf_len = (uint32_t)n;
    }
}

void kivfs_sha256_final(struct kivfs_sha256 *c, struct kivfs_digest *out)
{
    uint64_t bits = c->total * 8;

    /* padding: 0x80, nuly, 8 B délky v bitech (big-endian) */
    c->buf[c->buf_len++] = 0x80;
    if (c->buf_len > 56) {
        memset(c->buf + c->buf_len, 0, 64 - c->buf_len);
        compress(c->h, c->buf);
        c->buf_len = 0;
    }
    memset(c->buf + c->buf_len, 0, 56 - c->buf_len);
    for (int i = 0; i < 8; i++)
        c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    compress(c->h, c->buf);

    for (int i = 0; i < 8; i++) {
        out->b[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out->b[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out->b[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out->b[4 * i + 3] = (uint8_t)c->h[i];
    }
    c->buf_len = 0;
}

void kivfs_sha256(const void *data, size_t n, struct kivfs_digest *out)
{
    struct kivfs_sha256 c;
    kivfs_sha256_init(&c);
    kivfs_sha256_update(&c, data, n);
    kivfs_sha256_final(&c, out);
}

int kivfs_digest_eq(const struct kivfs_digest *a, const struct kivfs_digest *b)
{
    return memcmp(a->b, b->b, KIVFS_DIGEST_LEN) == 0;
}

void kivfs_digest_hex(const struct kivfs_digest *d, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (unsigned i = 0; i < KIVFS_DIGEST_LEN; i++) {
        out[2 * i]     = hx[d->b[i] >> 4];
        out[2 * i + 1] = hx[d->b[i] & 15];
    }
    out[KIVFS_DIGEST_HEX_LEN] = '\0';
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int kivfs_digest_from_hex(const char *hex, struct kivfs_digest *out)
{
    struct kivfs_digest d;
    for (unsigned i = 0; i < KIVFS_DIGEST_LEN; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hi < 0 ? -1 : hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return KIVFS_EINVAL;               /* i krátký řetězec: '\0' není hex */
        d.b[i] = (uint8_t)(hi << 4 | lo);
    }
    if (hex[KIVFS_DIGEST_HEX_LEN] != '\0')
        return KIVFS_EINVAL;
    *out = d;
    return KIVFS_OK;
}
