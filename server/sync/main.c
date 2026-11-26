/*
 * kivfs-sync – modul SYNC (kap. 7.4): synchronizace požadavků mezi uzly.
 *
 * Klientské operace od Auth dostanou časovou značku Tcli hlasováním všech
 * uzlů (pruh CLI) a provádějí se sériově v jejím pořadí (origin.c);
 * DB zápisy (SYNC_SUBMIT od DB) jdou kolem v pruhu DB a provádějí se na
 * všech uzlech přes DB_APPLY (apply.c); zprávy Synců ostatních uzlů
 * obsluhuje peer.c, spolehlivé doručení COMMIT/ABORT outbox.c, úklid po
 * restartu původce resolve.c.
 * Stav a pravidla: sync/sync.h.
 *
 * Použití: kivfs-sync -c <soubor.ini>
 *   [sync] order_cli = yes|no   (výchozí yes)
 */
#include "sync/sync.h"
#include "proto/errors.h"
#include "proto/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* recent – dokončené operace                                          */
/* ------------------------------------------------------------------ */

void sync_recent_add(struct sync_state *st, uint32_t lane, const struct kivfs_op_id *op,
                     uint32_t state, uint64_t t)
{
    struct sync_recent *r = &st->recent[st->recent_next];
    r->lane  = lane;
    r->op    = *op;
    r->state = state;
    r->t     = t;
    st->recent_next = (st->recent_next + 1) % SYNC_RECENT_CAP;
}

const struct sync_recent *sync_recent_find(const struct sync_state *st, uint32_t lane,
                                           const struct kivfs_op_id *op)
{
    for (uint32_t i = 0; i < SYNC_RECENT_CAP; i++) {
        const struct sync_recent *r = &st->recent[i];
        if (r->op.node != 0 && r->lane == lane && kivfs_op_id_eq(&r->op, op))
            return r;                         /* node 0 = prázdný slot */
    }
    return NULL;
}

struct sync_node *sync_node_find(struct sync_state *st, uint32_t id)
{
    for (size_t i = 0; i < st->n_nodes; i++)
        if (st->nodes[i].id == id)
            return &st->nodes[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Život                                                               */
/* ------------------------------------------------------------------ */

static const char *const SYNC_KEYS[] = { "order_cli", NULL };

static int free_entry_cb(struct kivfs_lane_entry *e, void *arg)
{
    uint32_t lane = *(const uint32_t *)arg;
    if (e->mine && e->user != NULL) {
        if (lane == KIVFS_LANE_CLI) {         /* sync_cli_op */
            struct sync_cli_op *op = e->user;
            kivfs_pkt_free(&op->req);
        }
        free(e->user);                        /* sync_db_op nic dalšího nedrží */
        e->user = NULL;
    }
    return 0;
}

static void sync_fini(struct kivfs_mod *m)
{
    struct sync_state *st = kivfs_mod_user(m);
    if (st == NULL)
        return;
    for (int l = 0; l < KIVFS_LANE_COUNT; l++) {
        if (st->lane[l] == NULL)
            continue;
        uint32_t lane = (uint32_t)l;
        kivfs_lane_foreach(st->lane[l], free_entry_cb, &lane);
        kivfs_lane_destroy(st->lane[l]);
    }
    for (size_t i = 0; i < st->n_nodes; i++)
        free(st->nodes[i].out.q);
    while (st->resolves != NULL) {
        struct sync_resolve *r = st->resolves;
        st->resolves = r->next;
        free(r);
    }
    free(st);
    kivfs_mod_set_user(m, NULL);
}

/* Při chybě init uklidí sám – kostra po neúspěšném init fini nevolá. */
static int sync_init(struct kivfs_mod *m, const struct kivfs_config *cfg)
{
    const struct kivfs_modconf *mc = kivfs_mod_conf(m);
    struct sync_state *st = calloc(1, sizeof *st);
    if (st == NULL)
        return -1;
    kivfs_mod_set_user(m, st);
    st->m     = m;
    st->epoch = 1;                            /* pevný pohled */

    const char *oc = kivfs_config_get(cfg, "sync", "order_cli");
    if (oc == NULL || strcmp(oc, "yes") == 0)
        st->order_cli = 1;
    else if (strcmp(oc, "no") == 0)
        st->order_cli = 0;
    else {
        fprintf(stderr, "kivfs-sync: [sync] order_cli: '%s' (povoleno yes/no)\n", oc);
        goto fail;
    }
    st->op_timeout_ms    = mc->op_timeout_ms;
    st->round_timeout_ms = mc->op_timeout_ms / 4;
    st->vfs_timeout_ms   = mc->op_timeout_ms / 2;
    st->apply_timeout_ms = mc->op_timeout_ms / 4;   /* prozatím – řetěz v úkolu 4 */
    st->self_node        = mc->local_node;

    for (size_t i = 0; i < mc->node_count; i++) {
        if (mc->nodes[i].id == mc->local_node)
            continue;
        struct sync_node *n = &st->nodes[st->n_nodes++];
        n->id = mc->nodes[i].id;
        n->out.q = calloc(SYNC_OUTBOX_CAP, sizeof *n->out.q);
        if (n->out.q == NULL)
            goto fail;
    }

    struct kivfs_lane_limits lim = { SYNC_LANE_ENTRIES, SYNC_LANE_BYTES };
    uint64_t inc = kivfs_mod_incarnation(m);
    if (kivfs_lane_create(&st->lane[KIVFS_LANE_CLI], KIVFS_LANE_CLI, mc->local_node, inc, &lim) != KIVFS_OK
        || kivfs_lane_create(&st->lane[KIVFS_LANE_DB], KIVFS_LANE_DB, mc->local_node, inc, &lim) != KIVFS_OK)
        goto fail;
    return 0;
fail:
    sync_fini(m);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Požadavky a události                                                */
/* ------------------------------------------------------------------ */

static int on_request(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                      struct kivfs_packet *req)
{
    struct sync_state *st = kivfs_mod_user(m);
    uint16_t code = req->header.request_code;

    if (from->module == KIVFS_MOD_AUTH && code >= KIVFS_REQ_STAT && code <= KIVFS_REQ_RMDIR)
        return sync_cli_submit(st, from, req);
    if (from->module == KIVFS_MOD_DB && code == KIVFS_REQ_SYNC_SUBMIT)
        return sync_db_submit(st, from, req);

    if (from->module == KIVFS_MOD_SYNC) {
        switch (code) {
        case KIVFS_REQ_SYNC_PROPOSE: return sync_on_propose(st, from, req);
        case KIVFS_REQ_SYNC_COMMIT:  return sync_on_commit(st, from, req);
        case KIVFS_REQ_SYNC_ABORT:   return sync_on_abort(st, from, req);
        case KIVFS_REQ_SYNC_QUERY:   return sync_on_query(st, from, req);
        case KIVFS_REQ_SYNC_CLOCK:   return sync_on_clock(st, from, req);
        default:                     break;
        }
    }
    return 0;
}

static void on_peer(struct kivfs_mod *m, uint32_t module, uint32_t node,
                    enum kivfs_peer_event ev, uint64_t incarnation)
{
    struct sync_state *st = kivfs_mod_user(m);
    if (module == KIVFS_MOD_DB) {
        sync_db_on_link(st, ev, incarnation);
        return;
    }
    if (module != KIVFS_MOD_SYNC)
        return;
    struct sync_node *n = sync_node_find(st, node);
    if (n == NULL)
        return;

    /* restart uzlu: jiná inkarnace, než jakou jsem viděl (oba směry) */
    if ((ev == KIVFS_PEER_LINK_UP || ev == KIVFS_PEER_IN_UP) && incarnation != 0) {
        if (n->incarnation != 0 && n->incarnation != incarnation) {
            n->incarnation = incarnation;
            sync_resolve_origin(st, node, incarnation);
        } else {
            n->incarnation = incarnation;
        }
    }

    if (ev == KIVFS_PEER_LINK_UP) {
        n->link_up = 1;
        sync_outbox_flush(st, n);
        sync_resolve_retry(st, node);
        sync_db_execute(st);                  /* ověření mohlo čekat na linku */
    } else if (ev == KIVFS_PEER_LINK_DOWN) {
        n->link_up = 0;
    }
}

int main(int argc, char **argv)
{
    static const struct kivfs_mod_ops ops = {
        .self       = KIVFS_MOD_SYNC,
        .extra_keys = SYNC_KEYS,
        .init       = sync_init,
        .fini       = sync_fini,
        .on_request = on_request,
        .on_peer    = on_peer,
    };
    return kivfs_module_main(argc, argv, &ops);
}
