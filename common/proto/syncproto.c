/*
 * KIVFS – zprávy synchronizace na drátě (implementace), viz syncproto.h
 */
#include "proto/syncproto.h"
#include "proto/errors.h"
#include "proto/protocol.h"

/* ------------------------------------------------------------------ */
/* Pomocné: op_id uvnitř větších struktur                              */
/* ------------------------------------------------------------------ */

static void put_op_id(uint8_t *b, const struct kivfs_op_id *op)
{
    kivfs_proto_put_u32(b,      op->node);
    kivfs_proto_put_u64(b + 4,  op->incarnation);
    kivfs_proto_put_u64(b + 12, op->seq);
}

static void get_op_id(const uint8_t *b, struct kivfs_op_id *op)
{
    op->node        = kivfs_proto_get_u32(b);
    op->incarnation = kivfs_proto_get_u64(b + 4);
    op->seq         = kivfs_proto_get_u64(b + 12);
}

/* Další záznam s délkou >= min (delší = novější verze, přebytek se
 * ignoruje). Při chybě vrátí reader na původní pozici. */
static int read_fixed(struct kivfs_pkt_reader *r, uint32_t min, const uint8_t **out)
{
    uint32_t pos = r->pos, len;
    int rc = kivfs_pkt_read_bytes(r, out, &len);
    if (rc != KIVFS_OK)
        return rc;
    if (len < min) {
        r->pos = pos;
        return KIVFS_EMALFORMED;
    }
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Zápis                                                               */
/* ------------------------------------------------------------------ */

int kivfs_sync_hdr_add(struct kivfs_packet *p, const struct kivfs_sync_hdr *h)
{
    uint8_t b[KIVFS_SYNC_HDR_WIRE_SIZE];
    kivfs_proto_put_u32(b,     h->epoch);
    kivfs_proto_put_u32(b + 4, h->lane);
    put_op_id(b + 8, &h->op);
    kivfs_proto_put_u64(b + 28, h->t);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

int kivfs_dbop_add(struct kivfs_packet *p, const struct kivfs_dbop *d)
{
    uint8_t b[KIVFS_DBOP_WIRE_SIZE];
    kivfs_proto_put_u32(b,     d->code);
    kivfs_proto_put_u32(b + 4, d->n_records);
    kivfs_proto_put_u64(b + 8, d->time_ms);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

int kivfs_arch_hdr_add(struct kivfs_packet *p, const struct kivfs_arch_hdr *a)
{
    uint8_t b[KIVFS_ARCH_HDR_WIRE_SIZE];
    kivfs_proto_put_u64(b,     a->index);
    kivfs_proto_put_u64(b + 8, a->t);
    put_op_id(b + 16, &a->op);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

int kivfs_sync_state_add(struct kivfs_packet *p, const struct kivfs_sync_state *s)
{
    uint8_t b[KIVFS_SYNC_STATE_WIRE_SIZE];
    kivfs_proto_put_u32(b,     s->state);
    kivfs_proto_put_u64(b + 4, s->t);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

int kivfs_sync_clock_add(struct kivfs_packet *p, const struct kivfs_sync_clock *c)
{
    uint8_t b[KIVFS_SYNC_CLOCK_WIRE_SIZE];
    kivfs_proto_put_u64(b,      c->tcli);
    kivfs_proto_put_u64(b + 8,  c->tdb);
    kivfs_proto_put_u64(b + 16, c->last_index);
    kivfs_proto_put_u64(b + 24, c->db_pending);
    return kivfs_pkt_add_bytes(p, b, sizeof b);
}

/* ------------------------------------------------------------------ */
/* Čtení                                                               */
/* ------------------------------------------------------------------ */

int kivfs_sync_hdr_read(struct kivfs_pkt_reader *r, struct kivfs_sync_hdr *h)
{
    const uint8_t *b;
    uint32_t pos = r->pos;
    int rc = read_fixed(r, KIVFS_SYNC_HDR_WIRE_SIZE, &b);
    if (rc != KIVFS_OK)
        return rc;
    uint32_t lane = kivfs_proto_get_u32(b + 4);
    if (lane >= KIVFS_LANE_COUNT) {
        r->pos = pos;
        return KIVFS_EMALFORMED;
    }
    h->epoch = kivfs_proto_get_u32(b);
    h->lane  = lane;
    get_op_id(b + 8, &h->op);
    h->t     = kivfs_proto_get_u64(b + 28);
    return KIVFS_OK;
}

int kivfs_dbop_read(struct kivfs_pkt_reader *r, struct kivfs_dbop *d)
{
    const uint8_t *b;
    int rc = read_fixed(r, KIVFS_DBOP_WIRE_SIZE, &b);
    if (rc != KIVFS_OK)
        return rc;
    d->code      = kivfs_proto_get_u32(b);
    d->n_records = kivfs_proto_get_u32(b + 4);
    d->time_ms   = kivfs_proto_get_u64(b + 8);
    return KIVFS_OK;
}

int kivfs_arch_hdr_read(struct kivfs_pkt_reader *r, struct kivfs_arch_hdr *a)
{
    const uint8_t *b;
    int rc = read_fixed(r, KIVFS_ARCH_HDR_WIRE_SIZE, &b);
    if (rc != KIVFS_OK)
        return rc;
    a->index = kivfs_proto_get_u64(b);
    a->t     = kivfs_proto_get_u64(b + 8);
    get_op_id(b + 16, &a->op);
    return KIVFS_OK;
}

int kivfs_sync_state_read(struct kivfs_pkt_reader *r, struct kivfs_sync_state *s)
{
    const uint8_t *b;
    int rc = read_fixed(r, KIVFS_SYNC_STATE_WIRE_SIZE, &b);
    if (rc != KIVFS_OK)
        return rc;
    s->state = kivfs_proto_get_u32(b);
    s->t     = kivfs_proto_get_u64(b + 4);
    return KIVFS_OK;
}

int kivfs_sync_clock_read(struct kivfs_pkt_reader *r, struct kivfs_sync_clock *c)
{
    const uint8_t *b;
    int rc = read_fixed(r, KIVFS_SYNC_CLOCK_WIRE_SIZE, &b);
    if (rc != KIVFS_OK)
        return rc;
    c->tcli       = kivfs_proto_get_u64(b);
    c->tdb        = kivfs_proto_get_u64(b + 8);
    c->last_index = kivfs_proto_get_u64(b + 16);
    c->db_pending = kivfs_proto_get_u64(b + 24);
    return KIVFS_OK;
}

int kivfs_dbop_skip(struct kivfs_pkt_reader *r, const uint8_t **start, uint32_t *len)
{
    uint32_t pos = r->pos;
    struct kivfs_dbop d;
    int rc = kivfs_dbop_read(r, &d);
    if (rc != KIVFS_OK)
        return rc;
    /* n_records přichází od protistrany: smyčka je omezená velikostí
     * payloadu (každý záznam spotřebuje aspoň 4 B), dřív skončí EEND. */
    for (uint32_t i = 0; i < d.n_records; i++) {
        const uint8_t *rec;
        uint32_t rlen;
        if (kivfs_pkt_read_bytes(r, &rec, &rlen) != KIVFS_OK) {
            r->pos = pos;
            return KIVFS_EMALFORMED;
        }
    }
    *start = r->data + pos;
    *len   = r->pos - pos;
    return KIVFS_OK;
}

int kivfs_dbop_copy(struct kivfs_packet *dst, struct kivfs_pkt_reader *r)
{
    /* nejdřív ověřit celou operaci na kopii readeru, ať dst nezůstane
     * s půlkou operace kvůli vadnému vstupu */
    struct kivfs_pkt_reader probe = *r;
    const uint8_t *start;
    uint32_t len;
    int rc = kivfs_dbop_skip(&probe, &start, &len);
    if (rc != KIVFS_OK)
        return rc;

    struct kivfs_dbop d;
    (void)kivfs_dbop_read(r, &d);           /* ověřeno výše */
    if ((rc = kivfs_dbop_add(dst, &d)) != KIVFS_OK)
        return rc;
    for (uint32_t i = 0; i < d.n_records; i++) {
        const uint8_t *rec;
        uint32_t rlen;
        (void)kivfs_pkt_read_bytes(r, &rec, &rlen);
        if ((rc = kivfs_pkt_add_bytes(dst, rec, rlen)) != KIVFS_OK)
            return rc;
    }
    return KIVFS_OK;
}
