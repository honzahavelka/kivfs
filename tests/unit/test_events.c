/*
 * Testy fronty událostí modulu (events.h), jen Linux.
 *
 * Testovaný "modul" = kivfs_ev + reactor v I/O vlákně; roli workera hraje
 * hlavní vlákno testu (kivfs_ev_pop). Protistranou odchozích spojení je
 * druhý reactor, který odpovídá přímo z callbacku (OP_ECHO) nebo vůbec
 * (OP_NOREPLY). Příchozí požadavky posílají blokující kanály (channel.h).
 *
 * Hlavní ověřovaná vlastnost: účtování tříd – limit se kontroluje už při
 * vzniku (kivfs_ev_request/_timer -> KIVFS_EAGAIN) a uvolní se až při pop,
 * takže garantované události se do fronty vždy vejdou.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net/channel.h"
#include "proto/errors.h"
#include "core/events.h"
#include "proto/packet.h"
#include "core/reactor.h"
#include "net/socket.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

#define CHECK_RC(expr, expected)                                           \
    do {                                                                   \
        int rc_ = (expr);                                                  \
        CHECK(rc_ == (expected));                                          \
        if (rc_ != (expected))                                             \
            fprintf(stderr, "     %s -> %d (%s)\n", #expr, rc_, kivfs_strerror(rc_)); \
    } while (0)

enum { OP_ECHO = 100, OP_NOREPLY = 101 };

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

static uint16_t pick_port(void)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = kivfs_sock_listen("127.0.0.1", 0, 1);
    if (fd < 0)
        return 0;
    getsockname(fd, (struct sockaddr *)&sa, &sl);
    kivfs_sock_close(&fd);
    return ntohs(sa.sin_port);
}

static void *run_thread(void *arg)
{
    kivfs_reactor_run(arg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Protistrana (echo server)                                           */
/* ------------------------------------------------------------------ */

struct peer {
    struct kivfs_reactor *r;
    pthread_t             th;
    uint16_t              port;
};

static void peer_packet(void *u, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    struct peer *p = u;
    if (pkt->header.request_code == OP_ECHO) {
        struct kivfs_packet resp;
        if (kivfs_pkt_init_response(&resp, pkt, 1, KIVFS_STATUS_OK) == KIVFS_OK)
            kivfs_reactor_send(p->r, id, &resp);
    }
    kivfs_pkt_free(pkt);
}

static void peer_start(struct peer *p)
{
    memset(p, 0, sizeof *p);
    struct kivfs_reactor_callbacks cb = { .on_packet = peer_packet, .user = p };
    struct kivfs_reactor_config cfg = { .bind_addr = "127.0.0.1", .backlog = 16, .max_conns = 16 };
    int rc = KIVFS_EIO;
    for (int i = 0; i < 5 && rc != KIVFS_OK; i++) {
        p->port = cfg.port = pick_port();
        rc = kivfs_reactor_create(&p->r, &cfg, &cb);
    }
    CHECK_RC(rc, KIVFS_OK);
    pthread_create(&p->th, NULL, run_thread, p->r);
}

static void peer_stop(struct peer *p)
{
    kivfs_reactor_stop(p->r);
    pthread_join(p->th, NULL);
    kivfs_reactor_destroy(p->r);
}

/* ------------------------------------------------------------------ */
/* Testovaný modul                                                     */
/* ------------------------------------------------------------------ */

struct mod {
    struct kivfs_ev      *ev;
    struct kivfs_reactor *r;
    pthread_t             th;
    uint16_t              port;
    int                   running;
};

static void mod_start(struct mod *m, const struct kivfs_ev_limits *lim)
{
    memset(m, 0, sizeof *m);
    struct kivfs_reactor_config cfg = { .bind_addr = "127.0.0.1", .backlog = 16, .max_conns = 8 };
    struct kivfs_reactor_callbacks cb;
    CHECK_RC(kivfs_ev_create(&m->ev, lim, &cfg), KIVFS_OK);
    kivfs_ev_callbacks(m->ev, &cb);
    int rc = KIVFS_EIO;
    for (int i = 0; i < 5 && rc != KIVFS_OK; i++) {
        m->port = cfg.port = pick_port();
        rc = kivfs_reactor_create(&m->r, &cfg, &cb);
    }
    CHECK_RC(rc, KIVFS_OK);
    kivfs_ev_attach(m->ev, m->r);
    pthread_create(&m->th, NULL, run_thread, m->r);
    m->running = 1;
}

/* Pořadí vypínání podle events.h; zbylé události (drain) vrátí do arr. */
static int mod_stop(struct mod *m, struct kivfs_event *arr, int max)
{
    int n = 0;
    kivfs_reactor_stop(m->r);
    pthread_join(m->th, NULL);
    kivfs_ev_shutdown(m->ev);
    struct kivfs_event e;
    while (kivfs_ev_pop(m->ev, &e) == KIVFS_OK) {
        if (arr != NULL && n < max)
            arr[n] = e;
        n++;
        kivfs_ev_event_free(&e);
        if (arr != NULL && n <= max)
            memset(&arr[n - 1].pkt, 0, sizeof arr[n - 1].pkt);
    }
    kivfs_reactor_destroy(m->r);
    kivfs_ev_destroy(m->ev);
    return n;
}

static struct kivfs_event pop(struct mod *m)
{
    struct kivfs_event e;
    memset(&e, 0, sizeof e);
    CHECK_RC(kivfs_ev_pop(m->ev, &e), KIVFS_OK);
    return e;
}

static int connect_to(struct mod *m, uint16_t port, kivfs_conn_id *id)
{
    struct sockaddr_storage a;
    socklen_t alen;
    int rc = kivfs_sock_resolve("127.0.0.1", port, &a, &alen);
    if (rc != KIVFS_OK)
        return rc;
    return kivfs_ev_connect(m->ev, (struct sockaddr *)&a, alen, id);
}

static int request(struct mod *m, kivfs_conn_id id, uint16_t op, uint64_t tag)
{
    struct kivfs_packet p;
    kivfs_pkt_init(&p, op, tag + 1000);
    return kivfs_ev_request(m->ev, id, &p, tag, 5000);
}

/* ------------------------------------------------------------------ */
/* Testy                                                               */
/* ------------------------------------------------------------------ */

/* USER, CONNECT, RESPONSE a TIMER projdou frontou ke "workerovi". */
static void test_basic(struct peer *p)
{
    struct mod m;
    mod_start(&m, NULL);

    CHECK_RC(kivfs_ev_post(m.ev, 42), KIVFS_OK);
    struct kivfs_event e = pop(&m);
    CHECK(e.type == KIVFS_EV_USER && e.tag == 42);

    kivfs_conn_id id;
    CHECK_RC(connect_to(&m, p->port, &id), KIVFS_OK);
    CHECK_RC(request(&m, id, OP_ECHO, 5), KIVFS_OK);
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_CONNECT && e.conn == id);
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_RESPONSE && e.tag == 5 && e.err == KIVFS_OK);
    CHECK(e.pkt.header.request_code == (OP_ECHO | KIVFS_RESPONSE_FLAG));
    kivfs_ev_event_free(&e);

    CHECK_RC(kivfs_ev_timer(m.ev, 10, 7), KIVFS_OK);
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_TIMER && e.tag == 7);

    kivfs_reactor_close_conn(m.r, id);
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_CLOSE && e.outgoing == 1 && e.reason == KIVFS_CLOSE_LOCAL);
    CHECK(mod_stop(&m, NULL, 0) == 0);
}

/* Garance: reactor uvolní místo už při callbacku, ale limit modulu se
 * uvolní až při pop – další request bez vyzvednutí výsledků -> EAGAIN. */
static void test_outstanding_limit(struct peer *p)
{
    struct kivfs_ev_limits lim = { .max_outstanding = 3 };
    struct mod m;
    mod_start(&m, &lim);

    kivfs_conn_id id;
    CHECK_RC(connect_to(&m, p->port, &id), KIVFS_OK);
    for (uint64_t t = 1; t <= 3; t++)
        CHECK_RC(request(&m, id, OP_ECHO, t), KIVFS_OK);
    sleep_ms(200);                            /* všechny odpovědi už jsou ve frontě */

    struct kivfs_packet q;
    kivfs_pkt_init(&q, OP_ECHO, 1);
    kivfs_pkt_add_u32(&q, 1);
    CHECK_RC(kivfs_ev_request(m.ev, id, &q, 4, 1000), KIVFS_EAGAIN);
    CHECK(q.data == NULL);                    /* paket převzat i při EAGAIN */

    struct kivfs_event e = pop(&m);
    CHECK(e.type == KIVFS_EV_CONNECT);
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_RESPONSE && e.tag == 1);
    kivfs_ev_event_free(&e);
    CHECK_RC(request(&m, id, OP_ECHO, 4), KIVFS_OK);   /* uvolněno popem */

    for (uint64_t t = 2; t <= 4; t++) {
        e = pop(&m);
        CHECK(e.type == KIVFS_EV_RESPONSE && e.tag == t && e.err == KIVFS_OK);
        kivfs_ev_event_free(&e);
    }
    mod_stop(&m, NULL, 0);
}

/* Limit časovačů a limit odchozích spojení. */
static void test_timer_and_conn_limit(struct peer *p)
{
    struct kivfs_ev_limits lim = { .max_timers = 1, .max_out_conns = 1 };
    struct mod m;
    mod_start(&m, &lim);

    CHECK_RC(kivfs_ev_timer(m.ev, 5, 1), KIVFS_OK);
    CHECK_RC(kivfs_ev_timer(m.ev, 5, 2), KIVFS_EAGAIN);
    struct kivfs_event e = pop(&m);
    CHECK(e.type == KIVFS_EV_TIMER && e.tag == 1);
    CHECK_RC(kivfs_ev_timer(m.ev, 5, 3), KIVFS_OK);
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_TIMER && e.tag == 3);

    kivfs_conn_id a, b;
    CHECK_RC(connect_to(&m, p->port, &a), KIVFS_OK);
    CHECK_RC(connect_to(&m, p->port, &b), KIVFS_EAGAIN);
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_CONNECT);
    kivfs_reactor_close_conn(m.r, a);
    CHECK_RC(connect_to(&m, p->port, &b), KIVFS_EAGAIN);   /* CLOSE ještě nevyzvednut */
    e = pop(&m);
    CHECK(e.type == KIVFS_EV_CLOSE && e.outgoing);
    CHECK_RC(connect_to(&m, p->port, &b), KIVFS_OK);
    mod_stop(&m, NULL, 0);
}

/* Plná třída REQUEST: druhý klient dostane BUSY hned z I/O vlákna,
 * první dostane odpověď, až ho worker vyzvedne. */
static void test_request_busy(void)
{
    struct kivfs_ev_limits lim = { .max_requests = 1 };
    struct mod m;
    mod_start(&m, &lim);

    struct kivfs_chan c1, c2;
    kivfs_chan_init(&c1);
    kivfs_chan_init(&c2);
    CHECK_RC(kivfs_chan_open(&c1, "127.0.0.1", m.port, 2000), KIVFS_OK);
    CHECK_RC(kivfs_chan_open(&c2, "127.0.0.1", m.port, 2000), KIVFS_OK);

    struct kivfs_packet r1, r2, resp;
    kivfs_pkt_init(&r1, OP_ECHO, 11);
    kivfs_pkt_init(&r2, OP_ECHO, 22);
    CHECK_RC(kivfs_chan_send(&c1, &r1), KIVFS_OK);
    sleep_ms(100);                            /* r1 je ve frontě, nikdo nepopuje */
    CHECK_RC(kivfs_chan_call(&c2, &r2, &resp), KIVFS_OK);
    CHECK(resp.header.return_value == KIVFS_STATUS_BUSY);
    kivfs_pkt_free(&resp);

    struct kivfs_event e = pop(&m);
    CHECK(e.type == KIVFS_EV_REQUEST && e.pkt.header.timestamp == 11);
    CHECK_RC(kivfs_pkt_init_response(&resp, &e.pkt, 5, KIVFS_STATUS_OK), KIVFS_OK);
    CHECK_RC(kivfs_reactor_send(m.r, e.conn, &resp), KIVFS_OK);
    kivfs_ev_event_free(&e);
    CHECK_RC(kivfs_chan_recv_response(&c1, &r1, &resp), KIVFS_OK);
    CHECK(resp.header.return_value == KIVFS_STATUS_OK);
    kivfs_pkt_free(&resp);

    kivfs_chan_close(&c1);
    kivfs_chan_close(&c2);
    mod_stop(&m, NULL, 0);
}

/* Zániky příchozích spojení jsou best-effort: nad limit se zahodí. */
static void test_in_close_best_effort(void)
{
    struct kivfs_ev_limits lim = { .max_in_closes = 1 };
    struct mod m;
    mod_start(&m, &lim);

    for (int i = 0; i < 3; i++) {
        struct kivfs_chan c;
        kivfs_chan_init(&c);
        CHECK_RC(kivfs_chan_open(&c, "127.0.0.1", m.port, 2000), KIVFS_OK);
        sleep_ms(20);
        kivfs_chan_close(&c);
    }
    sleep_ms(200);

    struct kivfs_event arr[16];
    int n = mod_stop(&m, arr, 16);
    int closes = 0;
    for (int i = 0; i < n && i < 16; i++)
        if (arr[i].type == KIVFS_EV_CLOSE && !arr[i].outgoing)
            closes++;
    CHECK(closes == 1);
}

/* Vypínání: rozpracovaný request dostane ESHUTDOWN, pak CLOSE odchozího
 * spojení; nevypršený časovač nic. Po drain vrací pop ESHUTDOWN. */
static void test_shutdown(struct peer *p)
{
    struct mod m;
    mod_start(&m, NULL);
    kivfs_conn_id id;
    CHECK_RC(connect_to(&m, p->port, &id), KIVFS_OK);
    CHECK_RC(request(&m, id, OP_NOREPLY, 9), KIVFS_OK);
    CHECK_RC(kivfs_ev_timer(m.ev, 60000, 1), KIVFS_OK);
    struct kivfs_event e = pop(&m);
    CHECK(e.type == KIVFS_EV_CONNECT);
    sleep_ms(50);

    struct kivfs_event arr[8];
    int n = mod_stop(&m, arr, 8);
    CHECK(n == 2);
    CHECK(arr[0].type == KIVFS_EV_RESPONSE && arr[0].tag == 9 && arr[0].err == KIVFS_ESHUTDOWN);
    CHECK(arr[1].type == KIVFS_EV_CLOSE && arr[1].outgoing && arr[1].reason == KIVFS_CLOSE_SHUTDOWN);
}

int main(void)
{
    alarm(60);                                /* pop blokuje – pojistka proti zaseknutí */
    kivfs_sock_ignore_sigpipe();
    struct peer p;
    peer_start(&p);

    test_basic(&p);
    test_outstanding_limit(&p);
    test_timer_and_conn_limit(&p);
    test_request_busy();
    test_in_close_best_effort();
    test_shutdown(&p);

    peer_stop(&p);
    printf("events: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
