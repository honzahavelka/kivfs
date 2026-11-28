/*
 * KIVFS – úložiště bloků v paměti (mock pro unit testy, viz store.h).
 * Pole bloků s lineárním hledáním – na testy stačí.
 */
#include "fs/store.h"

#include <stdlib.h>
#include <string.h>

#include "proto/errors.h"
#include "proto/protocol.h"

struct mblock {
    struct kivfs_digest h;
    uint8_t            *data;
    uint32_t            len;
};

struct mock_store {
    struct kivfs_store base;          /* musí být první */
    struct mblock     *v;
    size_t             n, cap;
};

static struct mblock *find(struct mock_store *s, const struct kivfs_digest *h)
{
    for (size_t i = 0; i < s->n; i++)
        if (kivfs_digest_eq(&s->v[i].h, h))
            return &s->v[i];
    return NULL;
}

static int16_t m_put(struct kivfs_store *bs, const struct kivfs_digest *h,
                     const void *data, uint32_t len)
{
    struct mock_store *s = (struct mock_store *)bs;
    struct kivfs_digest d;

    if (len == 0 || len > KIVFS_FS_BLOCK_MAX || data == NULL)
        return KIVFS_STATUS_MALFORMED;
    kivfs_sha256(data, len, &d);
    if (!kivfs_digest_eq(&d, h))
        return KIVFS_STATUS_MALFORMED;

    struct mblock *b = find(s, h);
    if (b != NULL && b->len == len)
        return KIVFS_STATUS_OK;                /* idempotence */

    uint8_t *copy = malloc(len);
    if (copy == NULL)
        return KIVFS_STATUS_NO_SPACE;          /* „místo“ mocku = paměť */
    memcpy(copy, data, len);

    if (b != NULL) {                           /* jiná délka = poškozený, nahradit */
        free(b->data);
        b->data = copy;
        b->len = len;
        return KIVFS_STATUS_OK;
    }
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 16;
        struct mblock *nv = realloc(s->v, ncap * sizeof *nv);
        if (nv == NULL) {
            free(copy);
            return KIVFS_STATUS_NO_SPACE;
        }
        s->v = nv;
        s->cap = ncap;
    }
    s->v[s->n].h = *h;
    s->v[s->n].data = copy;
    s->v[s->n].len = len;
    s->n++;
    return KIVFS_STATUS_OK;
}

static int16_t m_get(struct kivfs_store *bs, const struct kivfs_digest *h,
                     void *buf, uint32_t cap, uint32_t *len, int verify)
{
    struct mock_store *s = (struct mock_store *)bs;
    struct mblock *b = find(s, h);
    (void)verify;                              /* v paměti se data nepoškodí */
    if (b == NULL)
        return KIVFS_STATUS_NOT_FOUND;
    if (cap < b->len)
        return KIVFS_STATUS_INTERNAL;
    memcpy(buf, b->data, b->len);
    *len = b->len;
    return KIVFS_STATUS_OK;
}

static int16_t m_has(struct kivfs_store *bs, const struct kivfs_digest *h, uint32_t *len)
{
    struct mblock *b = find((struct mock_store *)bs, h);
    if (b == NULL)
        return KIVFS_STATUS_NOT_FOUND;
    if (len != NULL)
        *len = b->len;
    return KIVFS_STATUS_OK;
}

static int16_t m_remove(struct kivfs_store *bs, const struct kivfs_digest *h)
{
    struct mock_store *s = (struct mock_store *)bs;
    struct mblock *b = find(s, h);
    if (b == NULL)
        return KIVFS_STATUS_NOT_FOUND;
    free(b->data);
    *b = s->v[--s->n];                         /* pořadí nehraje roli */
    return KIVFS_STATUS_OK;
}

static void m_close(struct kivfs_store *bs)
{
    struct mock_store *s = (struct mock_store *)bs;
    if (s == NULL)
        return;
    for (size_t i = 0; i < s->n; i++)
        free(s->v[i].data);
    free(s->v);
    free(s);
}

static const struct kivfs_store_ops mock_ops = {
    .put = m_put, .get = m_get, .has = m_has, .remove = m_remove, .close = m_close
};

int kivfs_store_mock_open(struct kivfs_store **out)
{
    struct mock_store *s = calloc(1, sizeof *s);
    if (s == NULL)
        return KIVFS_ENOMEM;
    s->base.ops = &mock_ops;
    *out = &s->base;
    return KIVFS_OK;
}
