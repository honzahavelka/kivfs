/*
 * KIVFS – Sync: úklid PROPOSED položek po restartu jejich původce
 * (viz sync.h, „RESTART PŮVODCE“).
 *
 * Starý běh původce už COMMIT ani ABORT nepošle. Zeptám se všech
 * ostatních uzlů (kromě původce – jeho nový běh nic neví):
 *   kdokoli má COMMITTED/APPLIED  -> provést s jeho T_final
 *   všichni odpověděli, nikdo nemá -> zahodit
 *   někdo neodpověděl              -> čekat (sync_resolve_retry při LINK_UP)
 */
#include "sync/sync.h"
#include "proto/errors.h"
#include "proto/protocol.h"

#include <stdlib.h>

struct pick_ctx {
    struct sync_state *st;
    uint32_t           lane, node;
    uint64_t           new_inc;
};

static void resolve_start(struct sync_state *st, struct sync_resolve *r);

static void resolve_remove(struct sync_state *st, struct sync_resolve *r)
{
    for (struct sync_resolve **pp = &st->resolves; *pp != NULL; pp = &(*pp)->next)
        if (*pp == r) {
            *pp = r->next;
            free(r);
            return;
        }
}

static void resolve_done(struct sync_state *st, struct sync_resolve *r)
{
    struct kivfs_lane *L = st->lane[r->lane];
    const struct kivfs_lane_entry *e = kivfs_lane_find(L, &r->op);

    if (e == NULL || e->state != KIVFS_LANE_PROPOSED) {
        /* mezitím vyřešeno jinak (COMMIT/ABORT přece dorazil) */
    } else if (r->found) {
        int rc = kivfs_lane_commit(L, &r->op, r->t);
        kivfs_mod_log(st->m, "úklid: operace node%u/%llu po restartu původce PROVEDENA (t %llu)%s",
                      (unsigned)r->op.node, (unsigned long long)r->op.seq,
                      (unsigned long long)r->t, rc == KIVFS_OK ? "" : " – commit selhal");
    } else if (r->missing == 0) {
        kivfs_lane_abort(L, &r->op, NULL);
        sync_recent_add(st, r->lane, &r->op, KIVFS_OPST_UNKNOWN, 0);
        kivfs_mod_log(st->m, "úklid: operace node%u/%llu po restartu původce ZAHOZENA",
                      (unsigned)r->op.node, (unsigned long long)r->op.seq);
    } else {
        kivfs_mod_log(st->m, "úklid: operace node%u/%llu čeká – %u uzlů neodpovědělo",
                      (unsigned)r->op.node, (unsigned long long)r->op.seq, (unsigned)r->missing);
        return;                                  /* zůstává v seznamu */
    }
    uint32_t lane = r->lane;
    resolve_remove(st, r);
    sync_execute_next(st, lane);
}

static void query_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct sync_state *st = kivfs_mod_user(m);
    struct sync_resolve *r = state;

    if (err != KIVFS_OK || resp->header.return_value != KIVFS_STATUS_OK) {
        r->missing++;
    } else {
        struct kivfs_pkt_reader rd;
        struct kivfs_sync_state s;
        uint64_t ref;
        kivfs_pkt_reader_init(&rd, resp);
        if (kivfs_pkt_read_u64(&rd, &ref) != KIVFS_OK || kivfs_sync_state_read(&rd, &s) != KIVFS_OK)
            r->missing++;
        else if (s.state == KIVFS_OPST_COMMITTED || s.state == KIVFS_OPST_APPLIED) {
            r->found = 1;
            r->t = s.t;
        }
    }
    if (--r->waiting == 0) {
        if (err == KIVFS_ESHUTDOWN)
            return;                              /* modul končí, seznam uvolní fini */
        resolve_done(st, r);
    }
}

static void resolve_start(struct sync_state *st, struct sync_resolve *r)
{
    r->waiting = 1;                              /* pojistka během rozesílání */
    r->missing = 0;
    r->found   = 0;
    for (size_t i = 0; i < st->n_nodes; i++) {
        struct sync_node *n = &st->nodes[i];
        if (n->id == r->origin)
            continue;
        if (!n->link_up) {
            r->missing++;
            continue;
        }
        struct kivfs_packet req;
        struct kivfs_sync_hdr h = { st->epoch, r->lane, r->op, 0 };
        kivfs_pkt_init(&req, KIVFS_REQ_SYNC_QUERY, 0);
        if (kivfs_sync_hdr_add(&req, &h) != KIVFS_OK) {
            kivfs_pkt_free(&req);
            r->missing++;
            continue;
        }
        if (kivfs_mod_call_node(st->m, n->id, &req, st->round_timeout_ms, query_cont, r) != KIVFS_OK)
            r->missing++;
        else
            r->waiting++;
    }
    if (--r->waiting == 0)
        resolve_done(st, r);
}

static int pick_cb(struct kivfs_lane_entry *e, void *arg)
{
    struct pick_ctx *c = arg;
    if (e->state != KIVFS_LANE_PROPOSED || e->op.node != c->node || e->op.incarnation == c->new_inc)
        return 0;
    for (struct sync_resolve *r = c->st->resolves; r != NULL; r = r->next)
        if (r->lane == c->lane && kivfs_op_id_eq(&r->op, &e->op))
            return 0;                            /* už se řeší */
    struct sync_resolve *r = calloc(1, sizeof *r);
    if (r == NULL)
        return 0;                                /* bez paměti nelze; položka zůstane PROPOSED */
    r->lane   = c->lane;
    r->op     = e->op;
    r->origin = c->node;
    r->next   = c->st->resolves;
    c->st->resolves = r;
    return 0;
}

void sync_resolve_origin(struct sync_state *st, uint32_t node, uint64_t new_incarnation)
{
    /* Pruh CLI cizí operace nedrží – úklid se týká jen pruhu DB. Seznam
     * se nejdřív sestaví (foreach nesmí frontu měnit), pak spustí. */
    struct pick_ctx c = { st, KIVFS_LANE_DB, node, new_incarnation };
    kivfs_lane_foreach(st->lane[KIVFS_LANE_DB], pick_cb, &c);
    kivfs_mod_log(st->m, "node%u restartoval (inkarnace %016llx) – úklid jeho rozpracovaných operací",
                  (unsigned)node, (unsigned long long)new_incarnation);
    sync_resolve_retry(st, 0);
}

void sync_resolve_retry(struct sync_state *st, uint32_t node)
{
    (void)node;                                  /* zkusit všechny čekající */
    struct sync_resolve *r = st->resolves, *next;
    for (; r != NULL; r = next) {
        next = r->next;                          /* resolve_start může r odebrat */
        if (r->waiting == 0)
            resolve_start(st, r);
    }
}
