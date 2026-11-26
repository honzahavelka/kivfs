/*
 * KIVFS – Sync: provádění pruhu DB na každém uzlu, vztah k lokální DB
 * a ověření po startu (viz sync.h, „PROVÁDĚNÍ PRUHU DB“, „VZTAH K DB“,
 * „START A OVĚŘENÍ“, „STOPPED“).
 *
 * Vše řídí sync_db_execute – volá se po každé události, která může
 * něco odblokovat (COMMIT, ABORT, potvrzení COMMIT, odpověď DB, LINK_UP,
 * časovač) a udělá jediný další krok: zjistit head DB, ověřit se
 * u ostatních, nebo vydat / zopakovat DB_APPLY. Najednou běží nejvýš
 * jedno DB_APPLY (sériově), jedno LOG_HEAD a jedno kolo SYNC_CLOCK.
 */
#include "sync/sync.h"
#include "proto/errors.h"
#include "proto/protocol.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Pomocné                                                             */
/* ------------------------------------------------------------------ */

static void update_mode(struct sync_state *st)
{
    struct sync_db *d = &st->db;
    if (d->mode == SYNC_DB_STOPPED)
        return;
    uint32_t m = (d->link_up && d->head_ok && d->verified) ? SYNC_DB_RUNNING : SYNC_DB_UNKNOWN;
    if (m != d->mode && m == SYNC_DB_RUNNING)
        kivfs_mod_log(st->m, "pruh DB: provádím (index %" PRIu64 ")",
                      kivfs_lane_last_index(st->lane[KIVFS_LANE_DB]));
    d->mode = m;
}

static void retry_cb(struct kivfs_mod *m, void *state)
{
    struct sync_state *st = state;
    (void)m;
    st->db.retry_armed = 0;
    sync_db_execute(st);
}

/* Další pokus za SYNC_DB_RETRY_MS (nejvýš jeden naplánovaný). */
static void arm_retry(struct sync_state *st)
{
    if (st->db.retry_armed)
        return;
    if (kivfs_mod_timer(st->m, SYNC_DB_RETRY_MS, retry_cb, st) == KIVFS_OK)
        st->db.retry_armed = 1;
}

void sync_db_reply(struct sync_state *st, struct sync_db_op *op, int16_t status,
                   const struct kivfs_packet *resp)
{
    struct kivfs_packet orig = { .header = op->orig };   /* init_response čte jen hlavičku */
    struct kivfs_packet out;
    if (kivfs_pkt_init_response(&out, &orig, kivfs_mod_next_ts(st->m), status) == KIVFS_OK) {
        if (resp != NULL && sync_copy_records(&out, resp, 1) != KIVFS_OK) {
            /* stav zápisu nejde předat celý -> výsledek neznámý */
            kivfs_pkt_free(&out);
            kivfs_pkt_init_response(&out, &orig, kivfs_mod_next_ts(st->m), KIVFS_STATUS_TIMEOUT);
        }
        kivfs_mod_reply(st->m, op->from, &out);
    }
    free(op);
}

/* STOPPED: vlastním čekajícím SUBMITům TIMEOUT (dohodnuté operace
 * proběhnou jinde; ty, jejichž kolo ještě běží, dostanou odpověď z kola). */
static int stop_cb(struct kivfs_lane_entry *e, void *arg)
{
    struct sync_state *st = arg;
    if (e->mine && e->user != NULL && e->state != KIVFS_LANE_PROPOSED) {
        sync_db_reply(st, e->user, KIVFS_STATUS_TIMEOUT, NULL);
        e->user = NULL;                          /* foreach smí měnit jen user */
    }
    return 0;
}

void sync_db_stop(struct sync_state *st, const char *fmt, ...)
{
    char why[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    if (st->db.mode == SYNC_DB_STOPPED) {
        kivfs_mod_log(st->m, "pruh DB (už zastaven): %s", why);
        return;
    }
    st->db.mode = SYNC_DB_STOPPED;
    kivfs_mod_log(st->m, "pruh DB ZASTAVEN – uzel potřebuje obnovu: %s", why);
    kivfs_lane_foreach(st->lane[KIVFS_LANE_DB], stop_cb, st);
}

/* ------------------------------------------------------------------ */
/* DB_LOG_HEAD                                                         */
/* ------------------------------------------------------------------ */

static void head_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct sync_state *st = state;
    struct sync_db *d = &st->db;
    struct kivfs_lane *L = st->lane[KIVFS_LANE_DB];
    struct kivfs_pkt_reader rd;
    uint64_t ref, h;

    d->head_inflight = 0;
    if (err == KIVFS_ESHUTDOWN)
        return;
    if (err != KIVFS_OK || resp->header.return_value != KIVFS_STATUS_OK) {
        kivfs_mod_log(m, "DB_LOG_HEAD selhal (%s)",
                      err != KIVFS_OK ? kivfs_strerror(err) : kivfs_status_str(resp->header.return_value));
        arm_retry(st);
        return;
    }
    kivfs_pkt_reader_init(&rd, resp);
    if (kivfs_pkt_read_u64(&rd, &ref) != KIVFS_OK || kivfs_pkt_read_u64(&rd, &h) != KIVFS_OK) {
        kivfs_mod_log(m, "DB_LOG_HEAD: neplatná odpověď");
        arm_retry(st);
        return;
    }
    if (d->mode == SYNC_DB_STOPPED)
        return;

    uint64_t last = kivfs_lane_last_index(L);
    if (!d->verified) {
        /* start: převzít (nic se ještě neprovedlo) */
        kivfs_lane_set_last_index(L, h);
    } else if (h == last || (d->exec != NULL && h == d->exec->index)) {
        /* stejná DB (reconnect); rozpracované APPLY se zopakuje */
    } else if (st->n_nodes == 0 && d->exec == NULL) {
        kivfs_mod_log(m, "DB má index %" PRIu64 ", Sync %" PRIu64 " – jediný uzel, převzato"
                      " (DB je jediný zdroj pravdy)", h, last);
        kivfs_lane_set_last_index(L, h);
    } else {
        sync_db_stop(st, "DB má index %" PRIu64 ", Sync %" PRIu64 " (DB ztratila data nebo díra)",
                     h, last);
        return;
    }
    d->head_ok    = 1;
    d->head_known = 1;
    sync_db_execute(st);
}

static void send_log_head(struct sync_state *st)
{
    struct kivfs_packet q;
    kivfs_pkt_init(&q, KIVFS_REQ_DB_LOG_HEAD, 0);
    int rc = kivfs_mod_call(st->m, KIVFS_MOD_DB, &q, st->apply_timeout_ms, head_cont, st);
    if (rc == KIVFS_OK)
        st->db.head_inflight = 1;
    else if (rc != KIVFS_ENOTREADY)              /* ENOTREADY: počká na LINK_UP */
        arm_retry(st);
}

/* ------------------------------------------------------------------ */
/* Ověření po startu (SYNC_CLOCK všem ostatním)                        */
/* ------------------------------------------------------------------ */

struct check {
    struct sync_state *st;
    uint32_t           waiting;
    int                ahead;      /* někdo má last_index > můj */
    int                busy;       /* rozdíl / rozpracované / neodpověděl */
    uint64_t           h;
};

static void check_done(struct sync_state *st, struct check *c)
{
    struct sync_db *d = &st->db;
    d->check_inflight = 0;
    if (d->mode == SYNC_DB_STOPPED) {
        /* mezitím zastaveno (COMMIT neznámé operace) */
    } else if (c->ahead) {
        sync_db_stop(st, "ověření po startu: jiný uzel má víc provedených operací než já (%" PRIu64 ")",
                     c->h);
    } else if (c->busy) {
        arm_retry(st);
    } else {
        d->verified = 1;
        kivfs_mod_log(st->m, "pruh DB ověřen u všech uzlů (index %" PRIu64 ")", c->h);
        sync_db_execute(st);
    }
    free(c);
}

static void clock_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct check *c = state;
    struct kivfs_sync_clock k;
    struct kivfs_pkt_reader rd;
    uint64_t ref;
    (void)m;

    if (err != KIVFS_OK || resp->header.return_value != KIVFS_STATUS_OK) {
        c->busy = 1;                             /* neodpověděl / neví svůj head */
    } else {
        kivfs_pkt_reader_init(&rd, resp);
        if (kivfs_pkt_read_u64(&rd, &ref) != KIVFS_OK || kivfs_sync_clock_read(&rd, &k) != KIVFS_OK)
            c->busy = 1;
        else if (k.last_index > c->h)
            c->ahead = 1;
        else if (k.last_index != c->h || k.db_pending != 0)
            c->busy = 1;
    }
    if (--c->waiting == 0) {
        if (err == KIVFS_ESHUTDOWN) {
            free(c);
            return;
        }
        check_done(c->st, c);
    }
}

static void start_check(struct sync_state *st)
{
    struct check *c = calloc(1, sizeof *c);
    if (c == NULL) {
        arm_retry(st);
        return;
    }
    c->st = st;
    c->h  = kivfs_lane_last_index(st->lane[KIVFS_LANE_DB]);
    c->waiting = 1;                              /* pojistka během rozesílání */
    st->db.check_inflight = 1;
    for (size_t i = 0; i < st->n_nodes; i++) {
        struct kivfs_packet q;
        kivfs_pkt_init(&q, KIVFS_REQ_SYNC_CLOCK, 0);
        if (kivfs_mod_call_node(st->m, st->nodes[i].id, &q, st->round_timeout_ms, clock_cont, c)
            == KIVFS_OK)
            c->waiting++;
        else
            c->busy = 1;                         /* linka dole: zkusit znovu */
    }
    if (--c->waiting == 0)
        check_done(st, c);
}

/* ------------------------------------------------------------------ */
/* DB_APPLY                                                            */
/* ------------------------------------------------------------------ */

static void send_apply(struct sync_state *st);

static void apply_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct sync_state *st = state;
    struct sync_db *d = &st->db;
    struct kivfs_lane_entry *e = d->exec;

    d->exec_inflight = 0;
    if (err == KIVFS_ESHUTDOWN || e == NULL)
        return;
    if (err != KIVFS_OK) {
        /* výsledek neznámý – APPLY se zopakuje (DB pozná opakování, R5):
         * ETIMEOUT hned, ECONNLOST po LINK_UP DB */
        kivfs_mod_log(m, "DB_APPLY indexu %" PRIu64 " bez odpovědi (%s) – zopakuji",
                      e->index, kivfs_strerror(err));
        sync_db_execute(st);
        return;
    }
    int16_t status = resp->header.return_value;
    if (status == KIVFS_STATUS_INTERNAL) {
        sync_db_stop(st, "DB_APPLY indexu %" PRIu64 " vrátil INTERNAL", e->index);
        return;                                  /* finish NE: díra v posloupnosti */
    }

    /* index spotřebován (OK i neúspěšná podmínka) */
    struct kivfs_op_id id = e->op;
    uint64_t t = e->t;
    void *user = kivfs_lane_finish(st->lane[KIVFS_LANE_DB], e);
    d->exec = NULL;
    sync_recent_add(st, KIVFS_LANE_DB, &id, KIVFS_OPST_APPLIED, t);
    if (user != NULL)                            /* vlastní: odpověď na SUBMIT */
        sync_db_reply(st, user, status, resp);
    sync_db_execute(st);
}

static void send_apply(struct sync_state *st)
{
    struct sync_db *d = &st->db;
    struct kivfs_lane_entry *e = d->exec;
    struct kivfs_packet q;
    struct kivfs_arch_hdr ah = { e->index, e->t, e->op };
    struct kivfs_pkt_reader pr = { e->payload, e->payload_len, 0 };

    kivfs_pkt_init(&q, KIVFS_REQ_DB_APPLY, 0);
    if (kivfs_arch_hdr_add(&q, &ah) != KIVFS_OK || kivfs_dbop_copy(&q, &pr) != KIVFS_OK) {
        kivfs_pkt_free(&q);
        arm_retry(st);                           /* nedostatek paměti */
        return;
    }
    int rc = kivfs_mod_call(st->m, KIVFS_MOD_DB, &q, st->apply_timeout_ms, apply_cont, st);
    if (rc == KIVFS_OK)
        d->exec_inflight = 1;
    else if (rc != KIVFS_ENOTREADY)              /* ENOTREADY: počká na LINK_UP */
        arm_retry(st);
}

/* ------------------------------------------------------------------ */
/* Řízení                                                              */
/* ------------------------------------------------------------------ */

void sync_db_execute(struct sync_state *st)
{
    struct sync_db *d = &st->db;

    if (d->mode == SYNC_DB_STOPPED || !d->link_up)
        return;
    if (!d->head_ok) {
        if (!d->head_inflight)
            send_log_head(st);
        return;
    }
    if (!d->verified) {
        if (st->n_nodes == 0) {
            d->verified = 1;                     /* není koho se ptát */
        } else {
            if (!d->check_inflight)
                start_check(st);
            return;
        }
    }
    update_mode(st);
    if (d->exec != NULL) {                       /* vydaná hlava: zopakovat APPLY */
        if (!d->exec_inflight)
            send_apply(st);
        return;
    }
    d->exec = kivfs_lane_next(st->lane[KIVFS_LANE_DB]);
    if (d->exec != NULL)
        send_apply(st);                          /* sériově: další až po odpovědi */
}

void sync_db_on_link(struct sync_state *st, enum kivfs_peer_event ev, uint64_t incarnation)
{
    struct sync_db *d = &st->db;
    (void)incarnation;                           /* head se ověří vždy */
    if (ev == KIVFS_PEER_LINK_UP) {
        d->link_up = 1;
        d->head_ok = 0;
        update_mode(st);
        sync_db_execute(st);
    } else if (ev == KIVFS_PEER_LINK_DOWN) {
        d->link_up = 0;
        d->head_ok = 0;
        update_mode(st);
    }
}
