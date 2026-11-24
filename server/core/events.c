/*
 * KIVFS – fronta událostí modulu (implementace), viz events.h
 * ===========================================================
 *
 * kivfs_queue (kapacita = součet limitů tříd) + čítače tříd pod vlastním
 * mutexem. Čítač se zvyšuje při rezervaci (wrapper nebo callback) a snižuje
 * až po kivfs_queue_pop – mezi pop a snížením čítač chvíli "přeceňuje",
 * což je bezpečný směr (spíš odmítneme, než přeplníme).
 *
 * Zámky: ev->lock se nikdy nedrží při volání reactoru ani fronty, takže
 * nemůže vzniknout cyklus se zámkem mailboxu reactoru ani fronty.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/events.h"
#include "proto/errors.h"
#include "util/queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...)  fprintf(stderr, "[events] " __VA_ARGS__)

#define DEFAULT_OUTSTANDING  256
#define DEFAULT_OUT_CONNS    64     /* stejné jako výchozí v reactoru */
#define DEFAULT_TIMERS       256
#define DEFAULT_USER         16

enum cls {
    CLS_REQUEST,      /* best-effort: plná -> BUSY */
    CLS_IN_CLOSE,     /* best-effort: plná -> zahodit */
    CLS_RESPONSE,     /* garantovaná */
    CLS_OUT_CONN,     /* garantovaná; 1 jednotka = spojení = až 2 události */
    CLS_TIMER,        /* garantovaná */
    CLS_USER,         /* garantovaná (kontroluje se při post) */
    CLS_COUNT,
    CLS_NONE = CLS_COUNT   /* událost bez vlastní jednotky (CONNECT) */
};

struct kivfs_ev {
    struct kivfs_queue   *q;
    struct kivfs_reactor *r;
    pthread_mutex_t       lock;
    uint32_t              lim[CLS_COUNT];
    uint32_t              used[CLS_COUNT];
};

/* ------------------------------------------------------------------ */
/* Účtování                                                            */
/* ------------------------------------------------------------------ */

static enum cls class_of(const struct kivfs_event *e)
{
    switch (e->type) {
    case KIVFS_EV_REQUEST:  return CLS_REQUEST;
    case KIVFS_EV_RESPONSE: return CLS_RESPONSE;
    case KIVFS_EV_CONNECT:  return CLS_NONE;
    case KIVFS_EV_CLOSE:    return e->outgoing ? CLS_OUT_CONN : CLS_IN_CLOSE;
    case KIVFS_EV_TIMER:    return CLS_TIMER;
    case KIVFS_EV_USER:     return CLS_USER;
    }
    return CLS_NONE;
}

/* Rezervuje jednotku třídy; 0 = limit vyčerpán. */
static int reserve(struct kivfs_ev *ev, enum cls c)
{
    int ok;
    pthread_mutex_lock(&ev->lock);
    ok = ev->used[c] < ev->lim[c];
    if (ok)
        ev->used[c]++;
    pthread_mutex_unlock(&ev->lock);
    return ok;
}

static void release(struct kivfs_ev *ev, enum cls c)
{
    if (c == CLS_NONE)
        return;
    pthread_mutex_lock(&ev->lock);
    if (ev->used[c] > 0)
        ev->used[c]--;
    pthread_mutex_unlock(&ev->lock);
}

/* Vloží garantovanou událost (místo je už rezervované). Nevejde-li se,
 * je to chyba v účtování -> raději hlasitý pád než tiše visící operace. */
static void push_guaranteed(struct kivfs_ev *ev, struct kivfs_event *e)
{
    int rc = kivfs_queue_try_push(ev->q, e);
    if (rc == KIVFS_OK)
        return;
    if (rc == KIVFS_ESHUTDOWN) {
        /* Fronta už je ukončená – porušené pořadí vypínání (events.h).
         * Worker už nic nezpracuje, nemá komu to doručit. */
        LOG("událost typu %d po kivfs_ev_shutdown zahozena (špatné pořadí vypínání)\n",
            (int)e->type);
        kivfs_ev_event_free(e);
        return;
    }
    LOG("BUG: garantovaná událost typu %d se nevešla do fronty\n", (int)e->type);
    abort();
}

/* ------------------------------------------------------------------ */
/* Callbacky reactoru (I/O vlákno)                                     */
/* ------------------------------------------------------------------ */

static void reply_busy(struct kivfs_ev *ev, kivfs_conn_id id, const struct kivfs_packet *req)
{
    struct kivfs_packet resp;
    if (ev->r == NULL)
        return;
    /* timestamp 0: odpověď vzniká mimo modul, bez jeho hodin */
    if (kivfs_pkt_init_response(&resp, req, 0, KIVFS_STATUS_BUSY) == KIVFS_OK)
        kivfs_reactor_send(ev->r, id, &resp);
}

static void cb_packet(void *u, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    struct kivfs_ev *ev = u;
    struct kivfs_event e;
    memset(&e, 0, sizeof e);
    e.type = KIVFS_EV_REQUEST;
    e.conn = id;
    e.pkt  = *pkt;                       /* převzetí paketu */

    if (!reserve(ev, CLS_REQUEST)) {
        reply_busy(ev, id, &e.pkt);
        kivfs_ev_event_free(&e);
        return;
    }
    if (kivfs_queue_try_push(ev->q, &e) != KIVFS_OK) {   /* jen ESHUTDOWN */
        release(ev, CLS_REQUEST);
        reply_busy(ev, id, &e.pkt);
        kivfs_ev_event_free(&e);
    }
}

static void cb_response(void *u, kivfs_conn_id id, uint64_t tag, int err,
                        struct kivfs_packet *resp)
{
    struct kivfs_ev *ev = u;
    struct kivfs_event e;
    memset(&e, 0, sizeof e);
    e.type = KIVFS_EV_RESPONSE;
    e.conn = id;
    e.tag  = tag;
    e.err  = err;
    if (resp != NULL)
        e.pkt = *resp;
    push_guaranteed(ev, &e);             /* jednotka rezervovaná v kivfs_ev_request */
}

static void cb_connect(void *u, kivfs_conn_id id)
{
    struct kivfs_ev *ev = u;
    struct kivfs_event e;
    memset(&e, 0, sizeof e);
    e.type = KIVFS_EV_CONNECT;
    e.conn = id;
    push_guaranteed(ev, &e);             /* místo v rámci jednotky OUT_CONN */
}

static void cb_timer(void *u, uint64_t tag)
{
    struct kivfs_ev *ev = u;
    struct kivfs_event e;
    memset(&e, 0, sizeof e);
    e.type = KIVFS_EV_TIMER;
    e.tag  = tag;
    push_guaranteed(ev, &e);
}

static void cb_close(void *u, kivfs_conn_id id, int outgoing,
                     enum kivfs_close_reason reason, int err)
{
    struct kivfs_ev *ev = u;
    struct kivfs_event e;
    memset(&e, 0, sizeof e);
    e.type     = KIVFS_EV_CLOSE;
    e.conn     = id;
    e.outgoing = outgoing;
    e.reason   = reason;
    e.err      = err;

    if (outgoing) {
        push_guaranteed(ev, &e);         /* jednotka rezervovaná v kivfs_ev_connect */
        return;
    }
    /* příchozí: best-effort (viz events.h) */
    if (!reserve(ev, CLS_IN_CLOSE))
        return;
    if (kivfs_queue_try_push(ev->q, &e) != KIVFS_OK)
        release(ev, CLS_IN_CLOSE);
}

/* ------------------------------------------------------------------ */
/* Veřejné API                                                         */
/* ------------------------------------------------------------------ */

static void free_elem(void *p)
{
    kivfs_ev_event_free(p);
}

void kivfs_ev_event_free(struct kivfs_event *e)
{
    if (e != NULL)
        kivfs_pkt_free(&e->pkt);
}

int kivfs_ev_create(struct kivfs_ev **out, const struct kivfs_ev_limits *lim,
                    const struct kivfs_reactor_config *rcfg)
{
    if (out == NULL)
        return KIVFS_EINVAL;
    *out = NULL;
    if (rcfg == NULL || rcfg->max_conns == 0)
        return KIVFS_EINVAL;

    struct kivfs_ev_limits l;
    if (lim != NULL)
        l = *lim;
    else
        memset(&l, 0, sizeof l);

    struct kivfs_ev *ev = calloc(1, sizeof *ev);
    if (ev == NULL)
        return KIVFS_ENOMEM;

#define DEF(v, d) ((v) != 0 ? (v) : (d))
    ev->lim[CLS_REQUEST]  = DEF(l.max_requests,    rcfg->max_conns);
    ev->lim[CLS_IN_CLOSE] = DEF(l.max_in_closes,   rcfg->max_conns);
    ev->lim[CLS_RESPONSE] = DEF(l.max_outstanding, DEFAULT_OUTSTANDING);
    ev->lim[CLS_OUT_CONN] = DEF(l.max_out_conns,   DEF(rcfg->max_out_conns, DEFAULT_OUT_CONNS));
    ev->lim[CLS_TIMER]    = DEF(l.max_timers,      DEFAULT_TIMERS);
    ev->lim[CLS_USER]     = DEF(l.max_user,        DEFAULT_USER);
#undef DEF

    /* Kapacita: každá jednotka = 1 místo, jen OUT_CONN = 2 (CONNECT + CLOSE). */
    uint64_t cap = 0;
    for (int c = 0; c < CLS_COUNT; c++)
        cap += (uint64_t)ev->lim[c] * (c == CLS_OUT_CONN ? 2 : 1);
    if (cap > 0x7FFFFFFFu) {
        free(ev);
        return KIVFS_EINVAL;
    }

    if (pthread_mutex_init(&ev->lock, NULL) != 0) {
        free(ev);
        return KIVFS_ENOMEM;
    }
    int rc = kivfs_queue_create(&ev->q, (size_t)cap, sizeof(struct kivfs_event), free_elem);
    if (rc != KIVFS_OK) {
        pthread_mutex_destroy(&ev->lock);
        free(ev);
        return rc;
    }
    *out = ev;
    return KIVFS_OK;
}

void kivfs_ev_callbacks(struct kivfs_ev *ev, struct kivfs_reactor_callbacks *cb)
{
    memset(cb, 0, sizeof *cb);
    cb->on_packet   = cb_packet;
    cb->on_connect  = cb_connect;
    cb->on_response = cb_response;
    cb->on_timer    = cb_timer;
    cb->on_close    = cb_close;
    cb->user        = ev;
    /* on_accept záměrně NULL – o spojení se modul dozví z HELLO */
}

void kivfs_ev_attach(struct kivfs_ev *ev, struct kivfs_reactor *r)
{
    ev->r = r;
}

int kivfs_ev_pop(struct kivfs_ev *ev, struct kivfs_event *out)
{
    int rc = kivfs_queue_pop(ev->q, out);
    if (rc == KIVFS_OK)
        release(ev, class_of(out));
    return rc;
}

void kivfs_ev_shutdown(struct kivfs_ev *ev)
{
    if (ev != NULL)
        kivfs_queue_shutdown(ev->q);
}

void kivfs_ev_destroy(struct kivfs_ev *ev)
{
    if (ev == NULL)
        return;
    kivfs_queue_destroy(ev->q);
    pthread_mutex_destroy(&ev->lock);
    free(ev);
}

/* ------------------------------------------------------------------ */
/* Wrappery – rezervace místa pro budoucí událost                      */
/* ------------------------------------------------------------------ */

int kivfs_ev_connect(struct kivfs_ev *ev, const struct sockaddr *addr, socklen_t addrlen,
                     kivfs_conn_id *out)
{
    if (out != NULL)
        *out = KIVFS_CONN_ID_INVALID;
    if (ev == NULL || ev->r == NULL)
        return KIVFS_EINVAL;
    if (!reserve(ev, CLS_OUT_CONN))
        return KIVFS_EAGAIN;
    int rc = kivfs_reactor_connect(ev->r, addr, addrlen, out);
    if (rc != KIVFS_OK)
        release(ev, CLS_OUT_CONN);       /* žádná událost nepřijde */
    return rc;
}

int kivfs_ev_request(struct kivfs_ev *ev, kivfs_conn_id id, struct kivfs_packet *req,
                     uint64_t tag, int timeout_ms)
{
    if (req == NULL)
        return KIVFS_EINVAL;
    if (ev == NULL || ev->r == NULL) {
        kivfs_pkt_free(req);
        return KIVFS_EINVAL;
    }
    if (!reserve(ev, CLS_RESPONSE)) {
        kivfs_pkt_free(req);             /* převzetí jako u reactoru */
        return KIVFS_EAGAIN;
    }
    int rc = kivfs_reactor_request(ev->r, id, req, tag, timeout_ms);
    if (rc != KIVFS_OK)
        release(ev, CLS_RESPONSE);
    return rc;
}

int kivfs_ev_timer(struct kivfs_ev *ev, int delay_ms, uint64_t tag)
{
    if (ev == NULL || ev->r == NULL)
        return KIVFS_EINVAL;
    if (!reserve(ev, CLS_TIMER))
        return KIVFS_EAGAIN;
    int rc = kivfs_reactor_timer(ev->r, delay_ms, tag);
    if (rc != KIVFS_OK)
        release(ev, CLS_TIMER);
    return rc;
}

int kivfs_ev_post(struct kivfs_ev *ev, uint64_t tag)
{
    if (ev == NULL)
        return KIVFS_EINVAL;
    if (!reserve(ev, CLS_USER))
        return KIVFS_EAGAIN;
    struct kivfs_event e;
    memset(&e, 0, sizeof e);
    e.type = KIVFS_EV_USER;
    e.tag  = tag;
    int rc = kivfs_queue_try_push(ev->q, &e);
    if (rc != KIVFS_OK) {
        release(ev, CLS_USER);
        if (rc == KIVFS_EAGAIN) {
            LOG("BUG: USER událost se nevešla do fronty\n");
            abort();
        }
    }
    return rc;
}
