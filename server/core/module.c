/*
 * KIVFS – společná kostra serverového modulu (implementace), viz module.h
 *
 * Tagy odchozích požadavků a časovačů (events.h je nevykládá):
 *   [63..56] druh  [55..32] index (linky/operace)  [31..0] generace
 * Generace se zvyšuje s každým pokusem o spojení / každou operací, takže
 * pozdní odpověď nebo časovač ze starého pokusu se pozná a zahodí.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/module.h"
#include "proto/diag.h"
#include "proto/errors.h"
#include "proto/hello.h"
#include "proto/protocol.h"
#include "core/reactor.h"
#include "net/socket.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HELLO_TIMEOUT_MS   2000
#define BACKOFF_MIN_MS     100
#define BACKOFF_MAX_MS     5000
#define MAX_OPS            64
#define MAX_CALLS          1024    /* rozpracovaná kivfs_mod_call */
#define MAX_MOD_TIMERS     64      /* čekající kivfs_mod_timer */
#define MAX_LINKS          (KIVFS_MAX_NODES + KIVFS_MOD_COUNT)

enum { TAG_START = 1, TAG_HELLO, TAG_RECONNECT, TAG_OP, TAG_CALL, TAG_TIMER };

#define TAG(kind, idx, gen) (((uint64_t)(kind) << 56) | ((uint64_t)((idx) & 0xFFFFFFu) << 32) \
                             | (uint32_t)(gen))
#define TAG_KIND(t) ((unsigned)((t) >> 56))
#define TAG_IDX(t)  ((uint32_t)(((t) >> 32) & 0xFFFFFFu))
#define TAG_GEN(t)  ((uint32_t)(t))

/* Odchozí linka na modul, který volám. */
struct link {
    uint32_t              module, node;
    struct kivfs_endpoint ep;
    int                   is_dep;       /* modul stejného uzlu (ready závisí na něm) */
    int                   status_child; /* ptát se ho při STATUS (strom, hello.h) */
    uint32_t              state;        /* enum kivfs_link_state */
    kivfs_conn_id         conn;
    uint32_t              gen;
    int                   backoff_ms;
    uint32_t              failures;     /* neúspěšné pokusy od posledního UP */
    uint64_t              peer_inc;
};

/* Příchozí spojení, indexováno slotem conn_id (< max_conns, reactor.h). */
struct peer {
    kivfs_conn_id conn;                 /* platnost záznamu = shoda celého id */
    int           hello;
    uint32_t      module, node;
    uint64_t      inc;
};

/* Rozpracovaný STATUS (kaskáda). */
struct op {
    int                 active;
    uint32_t            gen;
    kivfs_conn_id       in_conn;        /* komu odpovědět */
    struct kivfs_packet resp;           /* odpověď se skládá postupně */
    int                 waiting;        /* kolik odpovědí ještě chybí */
};

/* Rozpracované kivfs_mod_call. */
struct call {
    int             active;
    uint32_t        gen;
    kivfs_mod_cont  cont;
    void           *state;
};

/* Čekající kivfs_mod_timer. */
struct mtimer {
    int                 active;
    uint32_t            gen;
    kivfs_mod_timer_cb  cb;
    void               *state;
};

/* Stav kivfs_mod_forward: komu a na co odpovědět. */
struct fwd {
    kivfs_conn_id       to;
    struct kivfs_header orig;          /* hlavička původního požadavku (kód + ref) */
    int16_t             lost_status;   /* odesláno, ale bez odpovědi */
};

struct kivfs_mod {
    const struct kivfs_mod_ops *ops;
    void                       *user;  /* stav konkrétního modulu */
    struct kivfs_modconf        mc;
    struct kivfs_hello          me;
    struct kivfs_ev            *ev;
    struct kivfs_reactor       *r;
    uint64_t                    clock;  /* timestamp odesílaných paketů */

    struct link                 links[MAX_LINKS];
    size_t                      nlinks;
    struct peer                *peers;  /* [mc.max_conns] */
    struct op                   ops_tab[MAX_OPS];
    struct call                 calls[MAX_CALLS];
    size_t                      calls_hint;   /* kde začít hledat volný slot */
    struct mtimer               timers[MAX_MOD_TIMERS];
};

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

static void mlog_v(const struct kivfs_mod *m, const char *fmt, va_list ap)
{
    struct timespec ts;
    struct tm tm;
    char when[16];
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(when, sizeof when, "%H:%M:%S", &tm);

    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    fprintf(stderr, "%s.%03ld [node%u %s] %s\n", when, ts.tv_nsec / 1000000,
            (unsigned)m->mc.local_node, kivfs_module_name(m->mc.self), msg);
}

static void mlog(const struct kivfs_mod *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mlog_v(m, fmt, ap);
    va_end(ap);
}

void kivfs_mod_log(const struct kivfs_mod *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mlog_v(m, fmt, ap);
    va_end(ap);
}

static const char *who(uint32_t module, uint32_t node, char *buf, size_t n)
{
    if (module == KIVFS_MOD_CLIENT)
        snprintf(buf, n, "client");
    else
        snprintf(buf, n, "%s@node%u", kivfs_module_name((enum kivfs_module)module), (unsigned)node);
    return buf;
}

/* Oznámí modulu změnu spojení se serverovým modulem (ops->on_peer). */
static void notify_peer(struct kivfs_mod *m, uint32_t module, uint32_t node,
                        enum kivfs_peer_event ev, uint64_t inc)
{
    if (m->ops->on_peer != NULL && module != KIVFS_MOD_CLIENT)
        m->ops->on_peer(m, module, node, ev, inc);
}

static const char *close_reason_str(int reason)
{
    switch (reason) {
    case KIVFS_CLOSE_PEER:         return "protistrana zavřela";
    case KIVFS_CLOSE_IO_ERROR:     return "chyba spojení";
    case KIVFS_CLOSE_PROTOCOL:     return "porušení protokolu";
    case KIVFS_CLOSE_TIMEOUT:      return "timeout příjmu";
    case KIVFS_CLOSE_LOCAL:        return "zavřeno lokálně";
    case KIVFS_CLOSE_SHUTDOWN:     return "ukončování";
    case KIVFS_CLOSE_CONNECT:      return "connect selhal";
    case KIVFS_CLOSE_RESP_TIMEOUT: return "protistrana neodpověděla";
    default:                       return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Odpovědi                                                            */
/* ------------------------------------------------------------------ */

void kivfs_mod_reply_status(struct kivfs_mod *m, kivfs_conn_id conn,
                            const struct kivfs_packet *req, int16_t status)
{
    struct kivfs_packet resp;
    if (kivfs_pkt_init_response(&resp, req, ++m->clock, status) == KIVFS_OK)
        kivfs_reactor_send(m->r, conn, &resp);
}

static void reply_ping(struct kivfs_mod *m, kivfs_conn_id conn, const struct kivfs_packet *req)
{
    struct kivfs_packet resp;
    struct kivfs_pkt_reader rd;
    const uint8_t *b;
    uint32_t len;
    if (kivfs_pkt_init_response(&resp, req, ++m->clock, KIVFS_STATUS_OK) != KIVFS_OK)
        return;
    kivfs_pkt_reader_init(&rd, req);
    while (kivfs_pkt_read_bytes(&rd, &b, &len) == KIVFS_OK) {
        if (kivfs_pkt_add_bytes(&resp, b, len) != KIVFS_OK) {
            kivfs_pkt_free(&resp);
            kivfs_mod_reply_status(m, conn, req, KIVFS_STATUS_MALFORMED);
            return;
        }
    }
    kivfs_reactor_send(m->r, conn, &resp);
}

/* ------------------------------------------------------------------ */
/* Linky (odchozí spojení)                                             */
/* ------------------------------------------------------------------ */

static void link_schedule_retry(struct kivfs_mod *m, struct link *l)
{
    size_t idx = (size_t)(l - m->links);
    int rc = kivfs_ev_timer(m->ev, l->backoff_ms, TAG(TAG_RECONNECT, idx, l->gen));
    /* EIO = reactor už skončil (modul se ukončuje a protistrana zavřela
     * dřív, než přišel náš SHUTDOWN) – nic neobnovovat */
    if (rc != KIVFS_OK && rc != KIVFS_EIO)
        mlog(m, "BUG: nelze naplánovat nový pokus o spojení (%s)", kivfs_strerror(rc));
    l->backoff_ms *= 2;
    if (l->backoff_ms > BACKOFF_MAX_MS)
        l->backoff_ms = BACKOFF_MAX_MS;
}

/* Neúspěch pokusu: log jen první a pak každý 10., ať log nezaplaví
 * modul, jehož protistrana zrovna neběží. */
static void link_failed(struct kivfs_mod *m, struct link *l, const char *why)
{
    char b[48];
    l->failures++;
    if (l->failures == 1 || l->failures % 10 == 0)
        mlog(m, "%s nedostupný (%s), pokus %u, další za %d ms",
             who(l->module, l->node, b, sizeof b), why, (unsigned)l->failures, l->backoff_ms);
    l->state = KIVFS_LINK_DOWN;
    l->conn  = KIVFS_CONN_ID_INVALID;
    link_schedule_retry(m, l);
}

static void link_connect(struct kivfs_mod *m, struct link *l)
{
    size_t idx = (size_t)(l - m->links);
    struct sockaddr_storage a;
    socklen_t alen;

    l->gen++;
    /* BLOKUJE (DNS) – v Dockeru odpovídá lokální resolver; otevřený bod */
    if (kivfs_modconf_resolve(&l->ep, &a, &alen) != KIVFS_OK) {
        link_failed(m, l, "nelze přeložit adresu");
        return;
    }
    int rc = kivfs_ev_connect(m->ev, (struct sockaddr *)&a, alen, &l->conn);
    if (rc != KIVFS_OK) {
        link_failed(m, l, kivfs_strerror(rc));
        return;
    }
    l->state = KIVFS_LINK_CONNECTING;

    /* HELLO hned – odejde, jakmile se spojení naváže */
    struct kivfs_packet req;
    if (kivfs_hello_request(&req, ++m->clock, &m->me) != KIVFS_OK
        || kivfs_ev_request(m->ev, l->conn, &req, TAG(TAG_HELLO, idx, l->gen),
                            HELLO_TIMEOUT_MS) != KIVFS_OK)
        kivfs_reactor_close_conn(m->r, l->conn);   /* CLOSE -> nový pokus */
}

static void on_hello_response(struct kivfs_mod *m, struct kivfs_event *e)
{
    uint32_t idx = TAG_IDX(e->tag);
    if (idx >= m->nlinks)
        return;
    struct link *l = &m->links[idx];
    if (TAG_GEN(e->tag) != l->gen || e->conn != l->conn)
        return;                                    /* starý pokus */

    char b[48];
    const char *why = NULL;
    struct kivfs_hello h;

    if (e->err != KIVFS_OK)
        why = kivfs_strerror(e->err);
    else if (e->pkt.header.return_value != KIVFS_STATUS_OK)
        why = kivfs_status_str(e->pkt.header.return_value);
    else if (kivfs_hello_parse_response(&e->pkt, &h) != KIVFS_OK)
        why = "neplatná odpověď HELLO";
    else if (h.version != KIVFS_PROTO_VERSION)
        why = "jiná verze protokolu";
    else if (h.module != l->module || h.node_id != l->node)
        why = "na adrese odpovídá jiný modul/uzel (chyba konfigurace?)";
    else if (h.cluster_hash != m->me.cluster_hash)
        why = "jiný seznam uzlů v [cluster]";
    else if (h.incarnation == m->me.incarnation)
        why = "spojení sám na sebe";

    if (why != NULL) {
        /* jednou zalogovat; spojení zavřít -> CLOSE -> nový pokus s backoffem */
        if (l->failures == 0 || l->failures % 10 == 0)
            mlog(m, "HELLO s %s selhalo: %s", who(l->module, l->node, b, sizeof b), why);
        kivfs_reactor_close_conn(m->r, l->conn);
        return;
    }

    if (l->peer_inc != 0 && l->peer_inc != h.incarnation)
        mlog(m, "linka na %s nahoře – protistrana byla RESTARTOVÁNA (nová inkarnace %016llx)",
             who(l->module, l->node, b, sizeof b), (unsigned long long)h.incarnation);
    else
        mlog(m, "linka na %s nahoře (inkarnace %016llx)",
             who(l->module, l->node, b, sizeof b), (unsigned long long)h.incarnation);
    l->state      = KIVFS_LINK_UP;
    l->peer_inc   = h.incarnation;
    l->backoff_ms = BACKOFF_MIN_MS;
    l->failures   = 0;
    notify_peer(m, l->module, l->node, KIVFS_PEER_LINK_UP, l->peer_inc);
}

static void on_link_closed(struct kivfs_mod *m, struct link *l, const struct kivfs_event *e)
{
    char b[48];
    if (e->reason == KIVFS_CLOSE_SHUTDOWN) {       /* modul končí – nic neobnovovat */
        l->state = KIVFS_LINK_DOWN;
        l->conn  = KIVFS_CONN_ID_INVALID;
        return;
    }
    if (l->state == KIVFS_LINK_UP) {
        mlog(m, "linka na %s spadla (%s), znovu za %d ms",
             who(l->module, l->node, b, sizeof b), close_reason_str(e->reason), l->backoff_ms);
        l->state = KIVFS_LINK_DOWN;
        l->conn  = KIVFS_CONN_ID_INVALID;
        link_schedule_retry(m, l);
        notify_peer(m, l->module, l->node, KIVFS_PEER_LINK_DOWN, l->peer_inc);
    } else {
        link_failed(m, l, close_reason_str(e->reason));
    }
}

static int ready(const struct kivfs_mod *m)
{
    for (size_t i = 0; i < m->nlinks; i++)
        if (m->links[i].is_dep && m->links[i].state != KIVFS_LINK_UP)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* STATUS – kaskáda                                                    */
/* ------------------------------------------------------------------ */

static int status_timeout_ms(enum kivfs_module self)
{
    /* hloubka kaskády pod modulem × 1 s: volaný musí stihnout odpovědět
     * dřív, než vyprší timeout volajícího */
    switch (self) {
    case KIVFS_MOD_AUTH: return 3000;
    case KIVFS_MOD_SYNC: return 2000;
    default:             return 1000;
    }
}

static void build_diag(const struct kivfs_mod *m, struct kivfs_diag_module *d)
{
    memset(d, 0, sizeof *d);
    d->node        = m->mc.local_node;
    d->module      = m->mc.self;
    d->incarnation = m->me.incarnation;
    d->ready       = (uint32_t)ready(m);
    for (size_t i = 0; i < m->nlinks && d->n_out < KIVFS_DIAG_MAX_LINKS; i++) {
        const struct link *l = &m->links[i];
        d->out[d->n_out++] = (struct kivfs_diag_link){
            l->module, l->node, l->state, l->state == KIVFS_LINK_UP ? l->peer_inc : 0 };
    }
    for (uint32_t s = 0; s < m->mc.max_conns && d->n_in < KIVFS_DIAG_MAX_LINKS; s++)
        if (m->peers[s].hello && m->peers[s].module != KIVFS_MOD_CLIENT)
            d->in[d->n_in++] = (struct kivfs_diag_peer){ m->peers[s].module, m->peers[s].node };
}

static void op_finish(struct kivfs_mod *m, struct op *o)
{
    kivfs_reactor_send(m->r, o->in_conn, &o->resp);   /* převezme resp */
    o->active = 0;
}

static void status_start(struct kivfs_mod *m, kivfs_conn_id from, const struct kivfs_packet *req)
{
    size_t i;
    for (i = 0; i < MAX_OPS && m->ops_tab[i].active; i++) { }
    if (i == MAX_OPS) {
        kivfs_mod_reply_status(m, from, req, KIVFS_STATUS_BUSY);
        return;
    }
    struct op *o = &m->ops_tab[i];
    struct kivfs_diag_module d;
    build_diag(m, &d);
    if (kivfs_pkt_init_response(&o->resp, req, ++m->clock, KIVFS_STATUS_OK) != KIVFS_OK)
        return;
    if (kivfs_diag_add(&o->resp, &d) != KIVFS_OK) {
        kivfs_pkt_free(&o->resp);
        kivfs_mod_reply_status(m, from, req, KIVFS_STATUS_INTERNAL);
        return;
    }
    o->active  = 1;
    o->gen++;
    o->in_conn = from;
    o->waiting = 0;

    /* zeptat se jen dětí ve stromu STATUS (hello.h) – ne všech modulů,
     * které volám: SYNC <-> DB by kaskádu zacyklilo; ne Synců ostatních
     * uzlů */
    for (size_t k = 0; k < m->nlinks; k++) {
        struct link *l = &m->links[k];
        if (!l->status_child || l->state != KIVFS_LINK_UP)
            continue;
        struct kivfs_packet q;
        kivfs_pkt_init(&q, KIVFS_REQ_STATUS, ++m->clock);
        if (kivfs_ev_request(m->ev, l->conn, &q, TAG(TAG_OP, i, o->gen),
                             status_timeout_ms(m->mc.self)) == KIVFS_OK)
            o->waiting++;
    }
    if (o->waiting == 0)
        op_finish(m, o);
}

static void on_status_response(struct kivfs_mod *m, struct kivfs_event *e)
{
    uint32_t i = TAG_IDX(e->tag);
    if (i >= MAX_OPS)
        return;
    struct op *o = &m->ops_tab[i];
    if (!o->active || o->gen != TAG_GEN(e->tag))
        return;

    /* bloky volaného modulu připojit za své; chyba = modul v odpovědi chybí */
    if (e->err == KIVFS_OK && e->pkt.header.return_value == KIVFS_STATUS_OK) {
        struct kivfs_pkt_reader rd;
        const uint8_t *b;
        uint32_t len;
        uint64_t ref;
        kivfs_pkt_reader_init(&rd, &e->pkt);
        if (kivfs_pkt_read_u64(&rd, &ref) == KIVFS_OK)
            while (kivfs_pkt_read_bytes(&rd, &b, &len) == KIVFS_OK)
                if (kivfs_pkt_add_bytes(&o->resp, b, len) != KIVFS_OK)
                    break;
    }
    if (--o->waiting == 0)
        op_finish(m, o);
}

/* ------------------------------------------------------------------ */
/* API pro moduly: stav, odpovědi, volání závislostí                   */
/* ------------------------------------------------------------------ */

void *kivfs_mod_user(const struct kivfs_mod *m)            { return m->user; }
void  kivfs_mod_set_user(struct kivfs_mod *m, void *user)   { m->user = user; }
const struct kivfs_modconf *kivfs_mod_conf(const struct kivfs_mod *m) { return &m->mc; }
uint64_t kivfs_mod_next_ts(struct kivfs_mod *m)            { return ++m->clock; }
uint64_t kivfs_mod_incarnation(const struct kivfs_mod *m)  { return m->me.incarnation; }

void kivfs_mod_reply(struct kivfs_mod *m, kivfs_conn_id to, struct kivfs_packet *resp)
{
    kivfs_reactor_send(m->r, to, resp);            /* převezme resp vždy */
}

/* Společná část call/call_node: odeslání po lince l (NULL = neexistuje). */
static int call_on_link(struct kivfs_mod *m, struct link *l, struct kivfs_packet *req,
                        int timeout_ms, kivfs_mod_cont cont, void *state)
{
    if (l == NULL || cont == NULL) {
        kivfs_pkt_free(req);
        return KIVFS_EINVAL;
    }
    if (l->state != KIVFS_LINK_UP) {               /* nečekat na timeout */
        kivfs_pkt_free(req);
        return KIVFS_ENOTREADY;
    }

    /* volný slot – hledá se od posledního obsazeného (obvykle hned) */
    size_t idx = MAX_CALLS;
    for (size_t k = 0; k < MAX_CALLS; k++) {
        size_t i = (m->calls_hint + k) % MAX_CALLS;
        if (!m->calls[i].active) {
            idx = i;
            break;
        }
    }
    if (idx == MAX_CALLS) {
        kivfs_pkt_free(req);
        return KIVFS_EAGAIN;
    }
    struct call *c = &m->calls[idx];
    c->gen++;
    req->header.timestamp = ++m->clock;
    int rc = kivfs_ev_request(m->ev, l->conn, req, TAG(TAG_CALL, idx, c->gen), timeout_ms);
    if (rc != KIVFS_OK)
        return rc;                                 /* EAGAIN (fronta), ... – cont nepřijde */
    c->active  = 1;
    c->cont    = cont;
    c->state   = state;
    m->calls_hint = (idx + 1) % MAX_CALLS;
    return KIVFS_OK;
}

int kivfs_mod_call(struct kivfs_mod *m, enum kivfs_module to, struct kivfs_packet *req,
                   int timeout_ms, kivfs_mod_cont cont, void *state)
{
    struct link *l = NULL;
    for (size_t i = 0; i < m->nlinks; i++)
        if (m->links[i].is_dep && m->links[i].module == (uint32_t)to)
            l = &m->links[i];
    return call_on_link(m, l, req, timeout_ms, cont, state);
}

int kivfs_mod_call_node(struct kivfs_mod *m, uint32_t node, struct kivfs_packet *req,
                        int timeout_ms, kivfs_mod_cont cont, void *state)
{
    struct link *l = NULL;
    if (m->mc.self == KIVFS_MOD_SYNC)
        for (size_t i = 0; i < m->nlinks; i++)
            if (!m->links[i].is_dep && m->links[i].module == KIVFS_MOD_SYNC
                && m->links[i].node == node)
                l = &m->links[i];
    return call_on_link(m, l, req, timeout_ms, cont, state);
}

static void on_call_response(struct kivfs_mod *m, struct kivfs_event *e)
{
    uint32_t idx = TAG_IDX(e->tag);
    if (idx >= MAX_CALLS)
        return;
    struct call *c = &m->calls[idx];
    if (!c->active || c->gen != TAG_GEN(e->tag))
        return;
    c->active = 0;                                 /* cont smí hned volat znovu */
    c->cont(m, c->state, e->err, e->err == KIVFS_OK ? &e->pkt : NULL);
}

int kivfs_mod_timer(struct kivfs_mod *m, int delay_ms, kivfs_mod_timer_cb cb, void *state)
{
    if (delay_ms < 0 || cb == NULL)
        return KIVFS_EINVAL;
    uint32_t i;
    for (i = 0; i < MAX_MOD_TIMERS && m->timers[i].active; i++) { }
    if (i == MAX_MOD_TIMERS)
        return KIVFS_EAGAIN;
    struct mtimer *t = &m->timers[i];
    t->gen++;
    int rc = kivfs_ev_timer(m->ev, delay_ms, TAG(TAG_TIMER, i, t->gen));
    if (rc != KIVFS_OK)
        return rc == KIVFS_EAGAIN ? KIVFS_EAGAIN : rc;
    t->active = 1;
    t->cb     = cb;
    t->state  = state;
    return KIVFS_OK;
}

static void fwd_cont(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct fwd *f = state;
    struct kivfs_packet orig = { .header = f->orig };   /* init_response čte jen hlavičku */
    struct kivfs_packet out;
    int16_t status = err == KIVFS_OK ? resp->header.return_value : f->lost_status;

    if (kivfs_pkt_init_response(&out, &orig, ++m->clock, status) == KIVFS_OK) {
        if (err == KIVFS_OK) {
            /* záznamy za ref volaného -> za náš ref */
            struct kivfs_pkt_reader rd;
            const uint8_t *b;
            uint32_t len;
            uint64_t ref;
            kivfs_pkt_reader_init(&rd, resp);
            if (kivfs_pkt_read_u64(&rd, &ref) == KIVFS_OK) {
                while (kivfs_pkt_read_bytes(&rd, &b, &len) == KIVFS_OK) {
                    if (kivfs_pkt_add_bytes(&out, b, len) != KIVFS_OK) {
                        kivfs_pkt_free(&out);
                        kivfs_pkt_init_response(&out, &orig, ++m->clock, KIVFS_STATUS_INTERNAL);
                        break;
                    }
                }
            }
        }
        kivfs_reactor_send(m->r, f->to, &out);
    }
    free(f);
}

void kivfs_mod_forward(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                       const struct kivfs_packet *orig, struct kivfs_packet *fwd,
                       enum kivfs_module to, int timeout_ms, int16_t lost_status)
{
    struct fwd *f = malloc(sizeof *f);
    if (f == NULL) {
        kivfs_pkt_free(fwd);
        kivfs_mod_reply_status(m, from->conn, orig, KIVFS_STATUS_INTERNAL);
        return;
    }
    f->to   = from->conn;
    f->orig = orig->header;
    f->lost_status = lost_status;
    int rc = kivfs_mod_call(m, to, fwd, timeout_ms, fwd_cont, f);
    if (rc != KIVFS_OK) {
        kivfs_mod_reply_status(m, from->conn, orig,
                               rc == KIVFS_ENOTREADY ? KIVFS_STATUS_UNAVAILABLE
                               : rc == KIVFS_EAGAIN  ? KIVFS_STATUS_BUSY
                                                     : KIVFS_STATUS_INTERNAL);
        free(f);
    }
}

/* ------------------------------------------------------------------ */
/* Příchozí spojení                                                    */
/* ------------------------------------------------------------------ */

/* NULL = smí se připojit, jinak důvod odmítnutí. */
static const char *check_incoming(const struct kivfs_mod *m, const struct kivfs_hello *h)
{
    if (h->version != KIVFS_PROTO_VERSION)
        return "jiná verze protokolu";
    if (!kivfs_module_may_call(h->module, m->mc.self))
        return "tento modul se sem nesmí připojit";
    if (h->module == KIVFS_MOD_CLIENT)
        return h->node_id == KIVFS_NODE_ID_UNKNOWN ? NULL : "klient musí mít node_id 0";
    if (h->module == KIVFS_MOD_SYNC && m->mc.self == KIVFS_MOD_SYNC) {
        if (h->node_id == m->mc.local_node || kivfs_modconf_node(&m->mc, h->node_id) == NULL)
            return "Sync neznámého uzlu";
    } else if (h->node_id != m->mc.local_node) {
        return "modul jiného uzlu";
    }
    if (h->cluster_hash != m->me.cluster_hash)
        return "jiný seznam uzlů v [cluster]";
    if (h->incarnation == m->me.incarnation)
        return "spojení sám na sebe";
    return NULL;
}

static void on_hello(struct kivfs_mod *m, kivfs_conn_id conn, struct peer *p,
                     const struct kivfs_packet *req)
{
    struct kivfs_hello h;
    struct kivfs_packet resp;
    char b[48];
    const char *why;

    if (p->hello)
        why = "druhé HELLO na stejném spojení";
    else if (kivfs_hello_parse_request(req, &h) != KIVFS_OK)
        why = "neplatný formát HELLO";
    else
        why = check_incoming(m, &h);

    if (why != NULL) {
        mlog(m, "odmítnuto HELLO: %s", why);
        if (kivfs_hello_response(&resp, req, ++m->clock, KIVFS_STATUS_REJECTED, &m->me) == KIVFS_OK)
            kivfs_reactor_send(m->r, conn, &resp);
        kivfs_reactor_close_conn(m->r, conn);      /* zavře se po odeslání odpovědi */
        return;
    }

    /* Stejný modul se připojil znovu: platí nové spojení, staré zavřít
     * (po restartu protistrany staré spojení často ještě "visí"). */
    if (h.module != KIVFS_MOD_CLIENT) {
        for (uint32_t s = 0; s < m->mc.max_conns; s++) {
            struct peer *o = &m->peers[s];
            if (o == p || !o->hello || o->module != h.module || o->node != h.node_id)
                continue;
            mlog(m, "%s se připojil znovu (%s) – staré spojení zavírám",
                 who(h.module, h.node_id, b, sizeof b),
                 o->inc == h.incarnation ? "reconnect" : "RESTART, nová inkarnace");
            kivfs_reactor_close_conn(m->r, o->conn);
            memset(o, 0, sizeof *o);
        }
    }

    p->hello  = 1;
    p->module = h.module;
    p->node   = h.node_id;
    p->inc    = h.incarnation;
    if (h.module != KIVFS_MOD_CLIENT)
        mlog(m, "připojil se %s (inkarnace %016llx)",
             who(h.module, h.node_id, b, sizeof b), (unsigned long long)h.incarnation);
    if (kivfs_hello_response(&resp, req, ++m->clock, KIVFS_STATUS_OK, &m->me) == KIVFS_OK)
        kivfs_reactor_send(m->r, conn, &resp);
    notify_peer(m, p->module, p->node, KIVFS_PEER_IN_UP, p->inc);
}

static void on_request(struct kivfs_mod *m, struct kivfs_event *e)
{
    uint32_t slot = (uint32_t)e->conn;
    if (slot >= m->mc.max_conns) {                 /* reactor.h to vylučuje */
        kivfs_reactor_close_conn(m->r, e->conn);
        return;
    }
    struct peer *p = &m->peers[slot];
    if (p->conn != e->conn) {                      /* nové spojení na slotu */
        memset(p, 0, sizeof *p);
        p->conn = e->conn;
    }

    uint16_t code = e->pkt.header.request_code;
    if (code == KIVFS_REQ_HELLO) {
        on_hello(m, e->conn, p, &e->pkt);
        return;
    }
    if (!p->hello) {                               /* nejdřív se představ */
        kivfs_mod_reply_status(m, e->conn, &e->pkt, KIVFS_STATUS_ACCESS_DENIED);
        kivfs_reactor_close_conn(m->r, e->conn);
        return;
    }
    switch (code) {
    case KIVFS_REQ_PING:
        reply_ping(m, e->conn, &e->pkt);
        return;
    case KIVFS_REQ_STATUS:
        status_start(m, e->conn, &e->pkt);
        return;
    default:
        if (m->ops->on_request != NULL) {
            struct kivfs_mod_peer from = { p->conn, p->module, p->node, p->inc };
            if (m->ops->on_request(m, &from, &e->pkt))
                return;
        }
        kivfs_mod_reply_status(m, e->conn, &e->pkt, KIVFS_STATUS_UNKNOWN_REQ);
    }
}

static void on_close(struct kivfs_mod *m, const struct kivfs_event *e)
{
    if (e->outgoing) {
        for (size_t i = 0; i < m->nlinks; i++)
            if (m->links[i].conn == e->conn) {
                on_link_closed(m, &m->links[i], e);
                return;
            }
        return;
    }
    uint32_t slot = (uint32_t)e->conn;
    if (slot < m->mc.max_conns && m->peers[slot].conn == e->conn) {
        struct peer *p = &m->peers[slot];
        char b[48];
        struct peer gone = *p;
        memset(p, 0, sizeof *p);
        if (gone.hello && gone.module != KIVFS_MOD_CLIENT && e->reason != KIVFS_CLOSE_SHUTDOWN) {
            mlog(m, "odpojil se %s (%s)", who(gone.module, gone.node, b, sizeof b),
                 close_reason_str(e->reason));
            notify_peer(m, gone.module, gone.node, KIVFS_PEER_IN_DOWN, gone.inc);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static void handle_event(struct kivfs_mod *m, struct kivfs_event *e)
{
    switch (e->type) {
    case KIVFS_EV_REQUEST:
        on_request(m, e);
        break;
    case KIVFS_EV_RESPONSE:
        if (TAG_KIND(e->tag) == TAG_HELLO)
            on_hello_response(m, e);
        else if (TAG_KIND(e->tag) == TAG_OP)
            on_status_response(m, e);
        else if (TAG_KIND(e->tag) == TAG_CALL)
            on_call_response(m, e);
        break;
    case KIVFS_EV_TIMER:
        if (TAG_KIND(e->tag) == TAG_TIMER && TAG_IDX(e->tag) < MAX_MOD_TIMERS) {
            struct mtimer *t = &m->timers[TAG_IDX(e->tag)];
            if (t->active && t->gen == TAG_GEN(e->tag)) {
                t->active = 0;                     /* cb smí hned založit další */
                t->cb(m, t->state);
            }
        } else if (TAG_KIND(e->tag) == TAG_RECONNECT && TAG_IDX(e->tag) < m->nlinks) {
            struct link *l = &m->links[TAG_IDX(e->tag)];
            if (TAG_GEN(e->tag) == l->gen && l->state == KIVFS_LINK_DOWN)
                link_connect(m, l);
        }
        break;
    case KIVFS_EV_USER:
        if (e->tag == TAG_START)
            for (size_t i = 0; i < m->nlinks; i++)
                link_connect(m, &m->links[i]);
        break;
    case KIVFS_EV_CLOSE:
        on_close(m, e);
        break;
    case KIVFS_EV_CONNECT:
        break;                                     /* HELLO už je zařazené */
    }
}

static void *worker_main(void *arg)
{
    struct kivfs_mod *m = arg;
    struct kivfs_event e;
    while (kivfs_ev_pop(m->ev, &e) == KIVFS_OK) {
        handle_event(m, &e);
        kivfs_ev_event_free(&e);
    }
    return NULL;
}

/* Signály SIGINT/SIGTERM jsou ve všech vláknech blokované; čeká se na ně
 * tady (sigwait) a stop se volá z běžného vlákna. */
static void *signal_main(void *arg)
{
    struct kivfs_mod *m = arg;
    sigset_t set;
    int sig;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (sigwait(&set, &sig) == 0)
        kivfs_reactor_stop(m->r);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Start                                                               */
/* ------------------------------------------------------------------ */

/* Načte a zvaliduje konfiguraci a zavolá ops->init (ten může číst
 * extra_keys z cfg). */
static int load_config(struct kivfs_mod *m, const char *path)
{
    struct kivfs_config *c = NULL;
    char err[512];
    int rc = 0;
    if (kivfs_config_load(path, &c, err, sizeof err) != KIVFS_OK
        || kivfs_modconf_from(c, m->ops->self, m->ops->extra_keys, &m->mc,
                              err, sizeof err) != KIVFS_OK) {
        fprintf(stderr, "kivfs-%s: %s\n", kivfs_module_name(m->ops->self), err);
        rc = -1;
    } else if (m->ops->init != NULL && m->ops->init(m, c) != 0) {
        fprintf(stderr, "kivfs-%s: inicializace modulu selhala\n",
                kivfs_module_name(m->ops->self));
        rc = -1;
    }
    kivfs_config_free(c);
    return rc;
}

static void setup_links(struct kivfs_mod *m)
{
    for (int mod = KIVFS_MOD_AUTH; mod < KIVFS_MOD_COUNT; mod++) {
        if (!m->mc.has_dep[mod])
            continue;
        struct link *l = &m->links[m->nlinks++];
        l->module = (uint32_t)mod;
        l->node   = m->mc.local_node;
        l->ep     = m->mc.deps[mod];
        l->is_dep = 1;
        l->status_child = kivfs_module_status_child(m->mc.self, (uint32_t)mod);
    }
    if (m->mc.self == KIVFS_MOD_SYNC) {
        for (size_t i = 0; i < m->mc.node_count; i++) {
            if (m->mc.nodes[i].id == m->mc.local_node)
                continue;
            struct link *l = &m->links[m->nlinks++];
            l->module = KIVFS_MOD_SYNC;
            l->node   = m->mc.nodes[i].id;
            l->ep     = m->mc.nodes[i].sync;
        }
    }
    for (size_t i = 0; i < m->nlinks; i++) {
        m->links[i].state      = KIVFS_LINK_DOWN;
        m->links[i].backoff_ms = BACKOFF_MIN_MS;
    }
}

static void usage(const char *self)
{
    fprintf(stderr, "použití: kivfs-%s -c <soubor.ini>\n", self);
}

int kivfs_module_main(int argc, char **argv, const struct kivfs_mod_ops *ops)
{
    const char *name = kivfs_module_name(ops->self);
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            path = argv[++i];
        else {
            usage(name);
            return 1;
        }
    }
    if (path == NULL) {
        usage(name);
        return 1;
    }

    struct kivfs_mod *m = calloc(1, sizeof *m);
    if (m == NULL)
        return 1;
    m->ops = ops;
    /* inkarnace už před init – modul ji může potřebovat (Sync: op_id) */
    m->me.incarnation = kivfs_incarnation_new();
    if (load_config(m, path) != 0) {
        free(m);
        return 1;
    }
    m->peers = calloc(m->mc.max_conns, sizeof *m->peers);
    if (m->peers == NULL) {
        if (ops->fini != NULL)
            ops->fini(m);
        free(m);
        return 1;
    }

    uint32_t ids[KIVFS_MAX_NODES];
    for (size_t i = 0; i < m->mc.node_count; i++)
        ids[i] = m->mc.nodes[i].id;               /* modconf je drží seřazené */
    m->me.node_id      = m->mc.local_node;
    m->me.module       = ops->self;
    m->me.version      = KIVFS_PROTO_VERSION;
    m->me.cluster_hash = kivfs_cluster_hash(ids, m->mc.node_count);
    setup_links(m);

    kivfs_sock_ignore_sigpipe();
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);        /* zdědí všechna vlákna */

    struct kivfs_reactor_config rcfg;
    struct kivfs_reactor_callbacks cb;
    kivfs_modconf_reactor_cfg(&m->mc, &rcfg);
    int rc = kivfs_ev_create(&m->ev, NULL, &rcfg);
    if (rc == KIVFS_OK) {
        kivfs_ev_callbacks(m->ev, &cb);
        rc = kivfs_reactor_create(&m->r, &rcfg, &cb);
        if (rc != KIVFS_OK)
            mlog(m, "nelze naslouchat na portu %u: %s (%s)", (unsigned)rcfg.port,
                 kivfs_strerror(rc), strerror(errno));
    }
    if (rc != KIVFS_OK) {
        kivfs_ev_destroy(m->ev);
        if (ops->fini != NULL)
            ops->fini(m);
        free(m->peers);
        free(m);
        return 1;
    }
    kivfs_ev_attach(m->ev, m->r);

    mlog(m, "start: port %u, inkarnace %016llx, cluster %08x, linek %zu",
         (unsigned)rcfg.port, (unsigned long long)m->me.incarnation,
         (unsigned)m->me.cluster_hash, m->nlinks);

    pthread_t worker, sigth;
    pthread_create(&worker, NULL, worker_main, m);
    pthread_create(&sigth, NULL, signal_main, m);
    kivfs_ev_post(m->ev, TAG_START);

    rc = kivfs_reactor_run(m->r);                  /* hlavní vlákno = I/O */

    /* pořadí vypínání podle events.h */
    kivfs_ev_shutdown(m->ev);
    pthread_join(worker, NULL);
    pthread_kill(sigth, SIGTERM);                  /* probudit, pokud ještě čeká */
    pthread_join(sigth, NULL);
    for (size_t i = 0; i < MAX_OPS; i++)
        kivfs_pkt_free(&m->ops_tab[i].resp);
    if (ops->fini != NULL)
        ops->fini(m);
    mlog(m, "ukončen");
    kivfs_reactor_destroy(m->r);
    kivfs_ev_destroy(m->ev);
    free(m->peers);
    free(m);
    return rc == KIVFS_OK ? 0 : 1;
}
