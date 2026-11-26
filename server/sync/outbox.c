/*
 * KIVFS – Sync: spolehlivé doručení COMMIT/ABORT (viz sync.h, „SPOLEHLIVÉ
 * COMMIT/ABORT“).
 *
 * Outbox uzlu = kruhová fronta zpráv. Odesílá se od čela, nejvýš
 * SYNC_OUTBOX_INFLIGHT zpráv najednou. Odpověď se páruje podle id zprávy
 * (token v state volání), ne podle pořadí: když jedna zpráva vyprší
 * a odpověď na další dorazí, musí zůstat ta první. Potvrzená zpráva se
 * označí done a z čela se odebírají souvislé done zprávy.
 */
#include "sync/sync.h"
#include "proto/errors.h"
#include "proto/protocol.h"

#include <stdlib.h>

/* state jednoho odeslání */
struct out_token {
    struct sync_state *st;
    uint32_t           node;
    uint64_t           id;
};

static struct sync_out_msg *slot(struct sync_outbox *o, uint32_t i)
{
    return &o->q[(o->head + i) % SYNC_OUTBOX_CAP];
}

static struct sync_out_msg *find_msg(struct sync_outbox *o, uint64_t id)
{
    for (uint32_t i = 0; i < o->count; i++)
        if (slot(o, i)->id == id)
            return slot(o, i);
    return NULL;
}

/* Odebere z čela potvrzené zprávy. */
static void pop_done(struct sync_outbox *o)
{
    while (o->count > 0 && o->q[o->head].done) {
        o->head = (o->head + 1) % SYNC_OUTBOX_CAP;
        o->count--;
    }
}

int sync_outbox_reserve(struct sync_state *st)
{
    for (size_t i = 0; i < st->n_nodes; i++) {
        struct sync_outbox *o = &st->nodes[i].out;
        if (o->count + o->reserved >= SYNC_OUTBOX_CAP)
            return -1;
    }
    for (size_t i = 0; i < st->n_nodes; i++)
        st->nodes[i].out.reserved++;
    return 0;
}

static void out_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct out_token *tk = state;
    struct sync_state *st = tk->st;
    struct sync_node *n = sync_node_find(st, tk->node);
    uint64_t id = tk->id;
    free(tk);
    if (n == NULL)
        return;
    n->out.inflight--;

    struct sync_out_msg *msg = find_msg(&n->out, id);
    if (msg == NULL)
        return;                                 /* nemá nastat */
    msg->inflight = 0;

    if (err == KIVFS_ESHUTDOWN)
        return;                                 /* modul končí */
    if (err == KIVFS_OK) {
        int16_t status = resp->header.return_value;
        if (status != KIVFS_STATUS_OK)          /* příjemce zprávu odmítl – opakovat nemá smysl */
            kivfs_mod_log(m, "%s pro node%u odmítnut: %s (op node%u/%llu)",
                          msg->code == KIVFS_REQ_SYNC_COMMIT ? "COMMIT" : "ABORT",
                          (unsigned)n->id, kivfs_status_str(status),
                          (unsigned)msg->op.node, (unsigned long long)msg->op.seq);
        msg->done = 1;
        /* bod 13 a: potvrzení COMMIT vlastní DB operace (i odmítnutí –
         * sync.h) uvolní hold u původce */
        int acked = msg->code == KIVFS_REQ_SYNC_COMMIT && msg->lane == KIVFS_LANE_DB
                    && msg->op.node == st->self_node;
        struct kivfs_op_id op = msg->op;
        pop_done(&n->out);
        if (acked) {
            if (status != KIVFS_STATUS_OK)
                kivfs_mod_log(m, "node%u COMMIT odmítl – počítám jako potvrzení, uzel"
                              " potřebuje obnovu", (unsigned)n->id);
            sync_commit_acked(st, &op);
        }
    }
    /* chyba (timeout, pád spojení): zpráva zůstane, odešle se znovu níže
     * nebo při LINK_UP */
    sync_outbox_flush(st, n);
}

void sync_outbox_flush(struct sync_state *st, struct sync_node *n)
{
    struct sync_outbox *o = &n->out;
    if (!n->link_up)
        return;
    for (uint32_t i = 0; i < o->count && o->inflight < SYNC_OUTBOX_INFLIGHT; i++) {
        struct sync_out_msg *msg = slot(o, i);
        if (msg->done || msg->inflight)
            continue;

        struct kivfs_packet req;
        struct kivfs_sync_hdr h = { st->epoch, msg->lane, msg->op, msg->t };
        struct out_token *tk = malloc(sizeof *tk);
        kivfs_pkt_init(&req, msg->code, 0);
        if (tk == NULL || kivfs_sync_hdr_add(&req, &h) != KIVFS_OK) {
            free(tk);
            kivfs_pkt_free(&req);
            return;                             /* zkusí se příště */
        }
        tk->st = st; tk->node = n->id; tk->id = msg->id;
        int rc = kivfs_mod_call_node(st->m, n->id, &req, st->round_timeout_ms, out_cont, tk);
        if (rc != KIVFS_OK) {                   /* ENOTREADY / EAGAIN: cont nepřijde */
            free(tk);
            return;
        }
        msg->inflight = 1;
        o->inflight++;
    }
}

void sync_outbox_push(struct sync_state *st, struct sync_node *n, uint16_t code,
                      uint32_t lane, const struct kivfs_op_id *op, uint64_t t)
{
    struct sync_outbox *o = &n->out;
    if (o->reserved > 0)
        o->reserved--;
    if (o->count >= SYNC_OUTBOX_CAP) {          /* rezervace to vylučuje */
        kivfs_mod_log(st->m, "BUG: outbox pro node%u plný, zpráva ztracena", (unsigned)n->id);
        return;
    }
    struct sync_out_msg *msg = slot(o, o->count);
    msg->id       = ++o->next_id;
    msg->done     = 0;
    msg->inflight = 0;
    msg->code     = code;
    msg->lane     = lane;
    msg->op       = *op;
    msg->t        = t;
    o->count++;
    sync_outbox_flush(st, n);
}
