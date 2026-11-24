/*
 * KIVFS – datové struktury operací souborového systému (implementace),
 * viz fsproto.h
 */
#include "proto/fsproto.h"
#include "proto/errors.h"
#include "proto/protocol.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Zápis                                                               */
/* ------------------------------------------------------------------ */

int kivfs_attr_add(struct kivfs_packet *p, const struct kivfs_attr *a)
{
    uint8_t b[KIVFS_ATTR_WIRE_SIZE];
    kivfs_proto_put_u64(b,      a->id);
    kivfs_proto_put_u32(b + 8,  a->mode);
    kivfs_proto_put_u32(b + 12, a->uid);
    kivfs_proto_put_u32(b + 16, a->gid);
    kivfs_proto_put_u64(b + 20, a->size);
    kivfs_proto_put_u64(b + 28, a->mtime_ms);
    kivfs_proto_put_u64(b + 36, a->ctime_ms);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

int kivfs_ctx_add(struct kivfs_packet *p, const struct kivfs_ctx *c)
{
    uint8_t b[KIVFS_CTX_WIRE_SIZE];
    kivfs_proto_put_u32(b,     c->uid);
    kivfs_proto_put_u32(b + 4, c->gid);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

int kivfs_mknode_add(struct kivfs_packet *p, uint32_t mode, uint32_t uid, uint32_t gid)
{
    uint8_t b[KIVFS_MKNODE_WIRE_SIZE];
    kivfs_proto_put_u32(b,     mode);
    kivfs_proto_put_u32(b + 4, uid);
    kivfs_proto_put_u32(b + 8, gid);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

int kivfs_entry_add(struct kivfs_packet *p, uint64_t id, uint32_t mode,
                    const char *name, size_t name_len)
{
    uint8_t b[KIVFS_ENTRY_FIXED_SIZE + KIVFS_NAME_MAX];
    if (name_len > KIVFS_NAME_MAX)
        return KIVFS_EINVAL;
    kivfs_proto_put_u64(b,     id);
    kivfs_proto_put_u32(b + 8, mode);
    memcpy(b + KIVFS_ENTRY_FIXED_SIZE, name, name_len);
    return kivfs_pkt_add_bytes(p, b, (uint32_t)(KIVFS_ENTRY_FIXED_SIZE + name_len));
}

/* ------------------------------------------------------------------ */
/* Čtení                                                               */
/* ------------------------------------------------------------------ */

/* Další záznam s délkou >= min. Při chybě vrátí reader na původní pozici. */
static int read_fixed(struct kivfs_pkt_reader *r, uint32_t min,
                      const uint8_t **out, uint32_t *len)
{
    uint32_t pos = r->pos;
    int rc = kivfs_pkt_read_bytes(r, out, len);
    if (rc != KIVFS_OK)
        return rc;
    if (*len < min) {
        r->pos = pos;
        return KIVFS_EMALFORMED;
    }
    return KIVFS_OK;
}

int kivfs_attr_read(struct kivfs_pkt_reader *r, struct kivfs_attr *a)
{
    const uint8_t *b;
    uint32_t len;
    int rc = read_fixed(r, KIVFS_ATTR_WIRE_SIZE, &b, &len);
    if (rc != KIVFS_OK)
        return rc;
    a->id       = kivfs_proto_get_u64(b);
    a->mode     = kivfs_proto_get_u32(b + 8);
    a->uid      = kivfs_proto_get_u32(b + 12);
    a->gid      = kivfs_proto_get_u32(b + 16);
    a->size     = kivfs_proto_get_u64(b + 20);
    a->mtime_ms = kivfs_proto_get_u64(b + 28);
    a->ctime_ms = kivfs_proto_get_u64(b + 36);
    return KIVFS_OK;
}

int kivfs_ctx_read(struct kivfs_pkt_reader *r, struct kivfs_ctx *c)
{
    const uint8_t *b;
    uint32_t len;
    int rc = read_fixed(r, KIVFS_CTX_WIRE_SIZE, &b, &len);
    if (rc != KIVFS_OK)
        return rc;
    c->uid = kivfs_proto_get_u32(b);
    c->gid = kivfs_proto_get_u32(b + 4);
    return KIVFS_OK;
}

int kivfs_mknode_read(struct kivfs_pkt_reader *r, uint32_t *mode, uint32_t *uid, uint32_t *gid)
{
    const uint8_t *b;
    uint32_t len;
    int rc = read_fixed(r, KIVFS_MKNODE_WIRE_SIZE, &b, &len);
    if (rc != KIVFS_OK)
        return rc;
    *mode = kivfs_proto_get_u32(b);
    *uid  = kivfs_proto_get_u32(b + 4);
    *gid  = kivfs_proto_get_u32(b + 8);
    return KIVFS_OK;
}

int kivfs_entry_read(struct kivfs_pkt_reader *r, uint64_t *id, uint32_t *mode,
                     const char **name, uint32_t *name_len)
{
    const uint8_t *b;
    uint32_t len;
    int rc = read_fixed(r, KIVFS_ENTRY_FIXED_SIZE, &b, &len);
    if (rc != KIVFS_OK)
        return rc;
    if (len - KIVFS_ENTRY_FIXED_SIZE > KIVFS_NAME_MAX) {
        r->pos -= KIVFS_RECORD_HDR_SIZE + len;
        return KIVFS_EMALFORMED;
    }
    *id       = kivfs_proto_get_u64(b);
    *mode     = kivfs_proto_get_u32(b + 8);
    *name     = (const char *)(b + KIVFS_ENTRY_FIXED_SIZE);
    *name_len = len - KIVFS_ENTRY_FIXED_SIZE;
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Cesty                                                               */
/* ------------------------------------------------------------------ */

static int path_valid(const uint8_t *p, uint32_t len)
{
    if (len == 0 || len > KIVFS_PATH_MAX || p[0] != '/')
        return 0;
    if (len == 1)
        return 1;                       /* "/" */
    if (p[len - 1] == '/')
        return 0;                       /* "/a/" */

    uint32_t start = 1;                 /* začátek aktuální složky */
    for (uint32_t i = 1; i <= len; i++) {
        if (i < len && p[i] == '\0')
            return 0;
        if (i < len && p[i] != '/')
            continue;
        uint32_t clen = i - start;      /* složka p[start .. i) */
        if (clen == 0 || clen > KIVFS_NAME_MAX)
            return 0;                   /* "//" nebo dlouhé jméno */
        if ((clen == 1 && p[start] == '.')
            || (clen == 2 && p[start] == '.' && p[start + 1] == '.'))
            return 0;
        start = i + 1;
    }
    return 1;
}

int kivfs_path_read(struct kivfs_pkt_reader *r, char *dst)
{
    uint32_t pos = r->pos;
    const uint8_t *b;
    uint32_t len;
    int rc = kivfs_pkt_read_bytes(r, &b, &len);
    if (rc != KIVFS_OK)
        return rc;
    if (!path_valid(b, len)) {
        r->pos = pos;
        return KIVFS_EMALFORMED;
    }
    memcpy(dst, b, len);
    dst[len] = '\0';
    return KIVFS_OK;
}

int kivfs_path_split(const char *path, char *parent_dst, const char **name)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0')
        return KIVFS_EINVAL;            /* "/" (nebo neplatná cesta) */
    size_t plen = (size_t)(slash - path);
    if (plen == 0) {
        parent_dst[0] = '/';
        parent_dst[1] = '\0';
    } else {
        memcpy(parent_dst, path, plen);
        parent_dst[plen] = '\0';
    }
    *name = slash + 1;
    return KIVFS_OK;
}
