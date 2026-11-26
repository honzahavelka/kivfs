/*
 * KIVFS – Sync: strana původce – kolo hlasování (oba pruhy), provádění
 * klientských operací v pořadí Tcli a vstup DB operací (SYNC_SUBMIT)
 * (viz sync.h, „KOLO U PŮVODCE“, „PROVÁDĚNÍ“, „PRUH DB U PŮVODCE“).
 */
#include "sync/sync.h"
#include "proto/errors.h"
#include "proto/protocol.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Pomocné                                                             */
/* ------------------------------------------------------------------ */

/* Chyba odeslání (call vrátil ne-OK, cont nepřijde) -> stav pro klienta. */
static int16_t send_fail_status(int rc)
{
    return rc == KIVFS_EAGAIN ? KIVFS_STATUS_BUSY : KIVFS_STATUS_UNAVAILABLE;
}

/* Zapamatuje chybu kola; BUSY má přednost (klient má zkusit znovu hned). */
static void round_fail(struct sync_round *r, int16_t status)
{
    if (r->fail == 0 || status == KIVFS_STATUS_BUSY)
        r->fail = status;
}

/* Připojí záznamy src za ref (odpověď) nebo od začátku (požadavek). */
int sync_copy_records(struct kivfs_packet *dst, const struct kivfs_packet *src, int skip_ref)
{
    struct kivfs_pkt_reader rd;
    const uint8_t *b;
    uint32_t len;
    uint64_t ref;
    int rc;
    kivfs_pkt_reader_init(&rd, src);
    if (skip_ref && kivfs_pkt_read_u64(&rd, &ref) != KIVFS_OK)
        return KIVFS_EMALFORMED;
    while ((rc = kivfs_pkt_read_bytes(&rd, &b, &len)) == KIVFS_OK)
        if ((rc = kivfs_pkt_add_bytes(dst, b, len)) != KIVFS_OK)
            return rc;
    return rc == KIVFS_EEND ? KIVFS_OK : rc;
}

static void cli_op_free(struct sync_cli_op *op)
{
    if (op != NULL) {
        kivfs_pkt_free(&op->req);
        free(op);
    }
}

/* Odpověď čekajícímu požadavku pruhu (CLI -> Auth, DB -> SUBMIT) jen se
 * stavem; uvolní kontext. user smí být NULL (DB po sync_db_stop). */
static void lane_user_fail(struct sync_state *st, uint32_t lane, void *user, int16_t status);

/* Odpověď Auth jen se stavem a uvolnění kontextu. */
static void cli_reply_status(struct sync_state *st, struct sync_cli_op *op, int16_t status)
{
    kivfs_mod_reply_status(st->m, op->from, &op->req, status);
    cli_op_free(op);
}

static void lane_user_fail(struct sync_state *st, uint32_t lane, void *user, int16_t status)
{
    if (user == NULL)
        return;
    if (lane == KIVFS_LANE_CLI)
        cli_reply_status(st, user, status);
    else
        sync_db_reply(st, user, status, NULL);
}

/* Odstraní z rezervace outboxů místo kola, které skončilo dřív, než
 * cokoli rozeslalo (rezervace se jinak spotřebuje v outbox_push). */
static void unreserve(struct sync_state *st)
{
    for (size_t i = 0; i < st->n_nodes; i++)
        if (st->nodes[i].out.reserved > 0)
            st->nodes[i].out.reserved--;
}

/* ------------------------------------------------------------------ */
/* Kolo                                                                */
/* ------------------------------------------------------------------ */

static void round_finish(struct sync_state *st, struct sync_round *r)
{
    struct kivfs_lane *L = st->lane[r->lane];

    if (r->fail == 0) {
        /* bod 13 a: vlastní DB operaci provést až po potvrzení COMMIT
         * od všech ostatních uzlů (sync_commit_acked) */
        if (r->lane == KIVFS_LANE_DB && st->n_nodes > 0)
            kivfs_lane_hold(L, &r->op, (uint32_t)st->n_nodes);
        int rc = kivfs_lane_commit(L, &r->op, r->tmax);
        if (rc != KIVFS_OK)                      /* vlastní PROPOSED položka – nemá nastat */
            kivfs_mod_log(st->m, "BUG: commit vlastní operace selhal: %s", kivfs_strerror(rc));
        for (size_t i = 0; i < st->n_nodes; i++)
            sync_outbox_push(st, &st->nodes[i], KIVFS_REQ_SYNC_COMMIT, r->lane, &r->op, r->tmax);
    } else {
        void *user = NULL;
        kivfs_lane_abort(L, &r->op, &user);
        for (size_t i = 0; i < st->n_nodes; i++)
            sync_outbox_push(st, &st->nodes[i], KIVFS_REQ_SYNC_ABORT, r->lane, &r->op, 0);
        sync_recent_add(st, r->lane, &r->op, KIVFS_OPST_UNKNOWN, 0);
        lane_user_fail(st, r->lane, user, r->fail);   /* bez COMMIT jistě neproběhlo */
    }
    uint32_t lane = r->lane;
    free(r);
    sync_execute_next(st, lane);                 /* commit i abort může uvolnit hlavu */
}

static void propose_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct sync_state *st = kivfs_mod_user(m);
    struct sync_round *r = state;

    if (err != KIVFS_OK) {
        round_fail(r, KIVFS_STATUS_UNAVAILABLE);     /* timeout, pád spojení, ukončování */
    } else if (resp->header.return_value != KIVFS_STATUS_OK) {
        round_fail(r, resp->header.return_value == KIVFS_STATUS_BUSY ? KIVFS_STATUS_BUSY
                                                                     : KIVFS_STATUS_UNAVAILABLE);
    } else {
        struct kivfs_pkt_reader rd;
        uint64_t ref, t;
        kivfs_pkt_reader_init(&rd, resp);
        if (kivfs_pkt_read_u64(&rd, &ref) != KIVFS_OK || kivfs_pkt_read_u64(&rd, &t) != KIVFS_OK) {
            kivfs_mod_log(m, "neplatná odpověď na PROPOSE");
            round_fail(r, KIVFS_STATUS_UNAVAILABLE);
        } else if (t > r->tmax) {
            r->tmax = t;
        }
    }
    if (--r->waiting == 0)
        round_finish(st, r);
}

void sync_round_start(struct sync_state *st, uint32_t lane, const struct kivfs_lane_entry *e)
{
    struct sync_round *r = calloc(1, sizeof *r);
    if (r == NULL) {
        /* bez stavu kola nelze nic rozeslat: zrušit hned (nikdo PROPOSE nemá) */
        void *user = NULL;
        struct kivfs_op_id op = e->op;
        kivfs_lane_abort(st->lane[lane], &op, &user);
        unreserve(st);
        lane_user_fail(st, lane, user, KIVFS_STATUS_INTERNAL);
        return;
    }
    r->lane = lane;
    r->op   = e->op;
    r->tmax = e->t;
    r->waiting = 1;                              /* pojistka: kolo neskončí během rozesílání */

    for (size_t i = 0; i < st->n_nodes && r->fail == 0; i++) {
        struct kivfs_packet req;
        struct kivfs_sync_hdr h = { st->epoch, lane, e->op, e->t };
        int rc;
        kivfs_pkt_init(&req, KIVFS_REQ_SYNC_PROPOSE, 0);
        rc = kivfs_sync_hdr_add(&req, &h);
        if (rc == KIVFS_OK && lane == KIVFS_LANE_DB) {
            struct kivfs_pkt_reader pr = { e->payload, e->payload_len, 0 };
            rc = kivfs_dbop_copy(&req, &pr);
        }
        if (rc != KIVFS_OK) {
            kivfs_pkt_free(&req);
            round_fail(r, KIVFS_STATUS_INTERNAL);
            break;
        }
        rc = kivfs_mod_call_node(st->m, st->nodes[i].id, &req, st->round_timeout_ms,
                                 propose_cont, r);
        if (rc != KIVFS_OK)
            round_fail(r, send_fail_status(rc));  /* dalším už neposílat */
        else
            r->waiting++;
    }
    if (--r->waiting == 0)                       /* žádný uzel / nic neodešlo */
        round_finish(st, r);
}

/* ------------------------------------------------------------------ */
/* Provádění (pruh CLI)                                                */
/* ------------------------------------------------------------------ */

/* state volání VFS = položka pruhu (platná do finish) */
static void vfs_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct sync_state *st = kivfs_mod_user(m);
    struct kivfs_lane_entry *e = state;
    struct sync_cli_op *op = e->user;
    struct kivfs_packet out;

    /* Výsledek: odpověď VFS, nebo NEZNÁMÝ (VFS mohl zapsat) -> TIMEOUT */
    int16_t status = err == KIVFS_OK          ? resp->header.return_value
                   : err == KIVFS_ESHUTDOWN   ? KIVFS_STATUS_UNAVAILABLE
                                              : KIVFS_STATUS_TIMEOUT;
    if (kivfs_pkt_init_response(&out, &op->req, kivfs_mod_next_ts(m), status) == KIVFS_OK) {
        if (err == KIVFS_OK && sync_copy_records(&out, resp, 1) != KIVFS_OK) {
            kivfs_pkt_free(&out);
            kivfs_pkt_init_response(&out, &op->req, kivfs_mod_next_ts(m), KIVFS_STATUS_INTERNAL);
        }
        kivfs_mod_reply(m, op->from, &out);
    }
    struct kivfs_op_id id = e->op;
    uint64_t t = e->t;
    kivfs_lane_finish(st->lane[KIVFS_LANE_CLI], e);
    sync_recent_add(st, KIVFS_LANE_CLI, &id, KIVFS_OPST_APPLIED, t);
    cli_op_free(op);
    sync_execute_next(st, KIVFS_LANE_CLI);
}

void sync_execute_next(struct sync_state *st, uint32_t lane)
{
    if (lane == KIVFS_LANE_DB) {
        sync_db_execute(st);                     /* apply.c */
        return;
    }

    struct kivfs_lane *L = st->lane[KIVFS_LANE_CLI];
    struct kivfs_lane_entry *e;
    while ((e = kivfs_lane_next(L)) != NULL) {
        struct sync_cli_op *op = e->user;
        struct kivfs_packet fwd;
        int rc;

        kivfs_pkt_init(&fwd, op->req.header.request_code, 0);
        rc = sync_copy_records(&fwd, &op->req, 0);
        if (rc == KIVFS_OK) {
            rc = kivfs_mod_call(st->m, KIVFS_MOD_VFS, &fwd, st->vfs_timeout_ms, vfs_cont, e);
            if (rc == KIVFS_OK)
                return;                          /* sériově: další až po odpovědi VFS */
        } else {
            kivfs_pkt_free(&fwd);
        }
        /* do VFS se nedostal -> jistě neprovedeno */
        int16_t status = rc == KIVFS_ENOTREADY ? KIVFS_STATUS_UNAVAILABLE
                       : rc == KIVFS_EAGAIN    ? KIVFS_STATUS_BUSY
                                               : KIVFS_STATUS_INTERNAL;
        struct kivfs_op_id id = e->op;
        kivfs_lane_finish(L, e);
        sync_recent_add(st, KIVFS_LANE_CLI, &id, KIVFS_OPST_APPLIED, 0);
        cli_reply_status(st, op, status);
    }
}

/* ------------------------------------------------------------------ */
/* Vstup klientské operace                                             */
/* ------------------------------------------------------------------ */

static void forward_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct sync_cli_op *op = state;
    struct kivfs_packet out;
    int16_t status = err == KIVFS_OK        ? resp->header.return_value
                   : err == KIVFS_ESHUTDOWN ? KIVFS_STATUS_UNAVAILABLE
                                            : KIVFS_STATUS_TIMEOUT;
    if (kivfs_pkt_init_response(&out, &op->req, kivfs_mod_next_ts(m), status) == KIVFS_OK) {
        if (err == KIVFS_OK && sync_copy_records(&out, resp, 1) != KIVFS_OK) {
            kivfs_pkt_free(&out);
            kivfs_pkt_init_response(&out, &op->req, kivfs_mod_next_ts(m), KIVFS_STATUS_INTERNAL);
        }
        kivfs_mod_reply(m, op->from, &out);
    }
    cli_op_free(op);
}

int sync_cli_submit(struct sync_state *st, const struct kivfs_mod_peer *from,
                    struct kivfs_packet *req)
{
    struct sync_cli_op *op = calloc(1, sizeof *op);
    if (op == NULL) {
        kivfs_mod_reply_status(st->m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return 1;
    }
    /* požadavek převezmu celý (hlavička kvůli ref, payload pro VFS) */
    op->from = from->conn;
    op->req  = *req;
    req->data = NULL;
    req->capacity = 0;
    req->header.data_size = 0;

    if (!st->order_cli) {                        /* bez kola Tcli, rovnou do VFS */
        struct kivfs_packet fwd;
        kivfs_pkt_init(&fwd, op->req.header.request_code, 0);
        int rc = sync_copy_records(&fwd, &op->req, 0);
        if (rc != KIVFS_OK) {
            kivfs_pkt_free(&fwd);
            cli_reply_status(st, op, KIVFS_STATUS_INTERNAL);
            return 1;
        }
        rc = kivfs_mod_call(st->m, KIVFS_MOD_VFS, &fwd, st->vfs_timeout_ms, forward_cont, op);
        if (rc != KIVFS_OK)
            cli_reply_status(st, op, rc == KIVFS_ENOTREADY ? KIVFS_STATUS_UNAVAILABLE
                                   : rc == KIVFS_EAGAIN    ? KIVFS_STATUS_BUSY
                                                           : KIVFS_STATUS_INTERNAL);
        return 1;
    }

    if (sync_outbox_reserve(st) != 0) {           /* některý uzel dlouho nedostupný */
        cli_reply_status(st, op, KIVFS_STATUS_BUSY);
        return 1;
    }
    struct kivfs_lane_entry *e;
    int rc = kivfs_lane_begin(st->lane[KIVFS_LANE_CLI], NULL, 0, op, &e);
    if (rc != KIVFS_OK) {
        unreserve(st);
        cli_reply_status(st, op, rc == KIVFS_EAGAIN ? KIVFS_STATUS_BUSY : KIVFS_STATUS_INTERNAL);
        return 1;
    }
    sync_round_start(st, KIVFS_LANE_CLI, e);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Vstup DB operace (SYNC_SUBMIT od DB)                                */
/* ------------------------------------------------------------------ */

int sync_db_submit(struct sync_state *st, const struct kivfs_mod_peer *from,
                   struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    const uint8_t *payload;
    uint32_t len;

    kivfs_pkt_reader_init(&rd, req);
    if (kivfs_dbop_skip(&rd, &payload, &len) != KIVFS_OK || !kivfs_pkt_reader_at_end(&rd)
        || req->header.data_size > KIVFS_MAX_PAYLOAD - KIVFS_SYNC_PAYLOAD_RESERVE) {
        kivfs_mod_reply_status(st->m, from->conn, req, KIVFS_STATUS_MALFORMED);
        return 1;
    }
    /* neprovádím (DB dole, neověřeno, STOPPED) -> kolo by nemělo smysl */
    if (st->db.mode != SYNC_DB_RUNNING) {
        kivfs_mod_reply_status(st->m, from->conn, req, KIVFS_STATUS_UNAVAILABLE);
        return 1;
    }
    if (sync_outbox_reserve(st) != 0) {
        kivfs_mod_reply_status(st->m, from->conn, req, KIVFS_STATUS_BUSY);
        return 1;
    }
    struct sync_db_op *op = malloc(sizeof *op);
    if (op == NULL) {
        unreserve(st);
        kivfs_mod_reply_status(st->m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return 1;
    }
    op->from = from->conn;
    op->orig = req->header;

    struct kivfs_lane_entry *e;
    int rc = kivfs_lane_begin(st->lane[KIVFS_LANE_DB], payload, len, op, &e);  /* kopie */
    if (rc != KIVFS_OK) {
        unreserve(st);
        free(op);
        kivfs_mod_reply_status(st->m, from->conn, req,
                               rc == KIVFS_EAGAIN ? KIVFS_STATUS_BUSY : KIVFS_STATUS_INTERNAL);
        return 1;
    }
    sync_round_start(st, KIVFS_LANE_DB, e);
    return 1;
}

void sync_commit_acked(struct sync_state *st, const struct kivfs_op_id *op)
{
    /* ENOENT: operace už není (nemá nastat – s hold se neprovede) */
    if (kivfs_lane_release(st->lane[KIVFS_LANE_DB], op) == KIVFS_OK)
        sync_execute_next(st, KIVFS_LANE_DB);
}
