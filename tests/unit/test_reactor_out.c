/*
 * Testy ODCHOZÍCH spojení a časovačů reactoru (jen Linux).
 *
 * Zapojení: dva reactory v jednom procesu, každý ve vlastním I/O vlákně.
 *   "server" – odpovídá přímo z callbacků (bez workerů), operace:
 *       OP_ECHO     odpověď = ref + kopie záznamů
 *       OP_SLOW     odpoví za SLOW_MS (přes kivfs_reactor_timer)
 *       OP_NOREPLY  nikdy neodpoví
 *       OP_BADREF   odpoví se špatným ref
 *       OP_DROP     neodpoví a zavře spojení
 *   "klient" – testovaný: kivfs_reactor_connect / request / timer.
 *     Všechny jeho callbacky se zapisují do logu událostí; test pak čeká
 *     na konkrétní událost a kontroluje pořadí a invariant "každý request
 *     právě jeden on_response".
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "proto/errors.h"
#include "proto/packet.h"
#include "core/reactor.h"
#include "net/socket.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_checks, g_failed;

/* Podmínka se vyhodnotí MIMO zámek – může čekat na událost z I/O vlákna,
 * jehož callbacky CHECK také používají. */
#define CHECK(cond)                                                        \
    do {                                                                   \
        int ok_ = (cond) ? 1 : 0;                                          \
        pthread_mutex_lock(&g_lock);                                       \
        g_checks++;                                                        \
        if (!ok_) {                                                        \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
        pthread_mutex_unlock(&g_lock);                                     \
    } while (0)

#define CHECK_RC(expr, expected)                                           \
    do {                                                                   \
        int rc_ = (expr);                                                  \
        CHECK(rc_ == (expected));                                          \
        if (rc_ != (expected))                                             \
            fprintf(stderr, "     %s -> %d (%s)\n", #expr, rc_, kivfs_strerror(rc_)); \
    } while (0)

enum { OP_ECHO = 100, OP_SLOW = 101, OP_NOREPLY = 102, OP_BADREF = 103, OP_DROP = 104 };
#define SLOW_MS 300

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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
/* Server                                                              */
/* ------------------------------------------------------------------ */

#define SRV_SLOTS 16

struct server {
    struct kivfs_reactor *r;
    pthread_t             th;
    uint16_t              port;
    struct kivfs_packet   slow[SRV_SLOTS];   /* čekající OP_SLOW, index = slot */
    kivfs_conn_id         slow_id[SRV_SLOTS];
    int                   closes[KIVFS_CLOSE_RESP_TIMEOUT + 1];  /* pod g_lock */
};

static void reply(struct server *s, kivfs_conn_id id, struct kivfs_packet *req, uint64_t ref_delta)
{
    struct kivfs_packet resp;
    struct kivfs_pkt_reader rd;
    const uint8_t *b;
    uint32_t len;

    req->header.timestamp += ref_delta;       /* OP_BADREF: posunutý ref */
    if (kivfs_pkt_init_response(&resp, req, 1, KIVFS_STATUS_OK) != KIVFS_OK)
        return;
    kivfs_pkt_reader_init(&rd, req);
    while (kivfs_pkt_read_bytes(&rd, &b, &len) == KIVFS_OK)
        kivfs_pkt_add_bytes(&resp, b, len);
    kivfs_reactor_send(s->r, id, &resp);
}

static void srv_packet(void *u, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    struct server *s = u;
    uint32_t slot = (uint32_t)id;
    switch (pkt->header.request_code) {
    case OP_ECHO:   reply(s, id, pkt, 0); break;
    case OP_BADREF: reply(s, id, pkt, 7); break;
    case OP_SLOW:
        if (slot < SRV_SLOTS) {
            s->slow[slot] = *pkt;
            s->slow_id[slot] = id;
            memset(pkt, 0, sizeof *pkt);
            kivfs_reactor_timer(s->r, SLOW_MS, slot);
        }
        break;
    case OP_DROP:   kivfs_reactor_close_conn(s->r, id); break;
    default:        break;                    /* OP_NOREPLY */
    }
    kivfs_pkt_free(pkt);
}

static void srv_timer(void *u, uint64_t tag)
{
    struct server *s = u;
    reply(s, s->slow_id[tag], &s->slow[tag], 0);
    kivfs_pkt_free(&s->slow[tag]);
}

static void srv_close(void *u, kivfs_conn_id id, int outgoing,
                      enum kivfs_close_reason reason, int err)
{
    struct server *s = u;
    (void)id; (void)err;
    CHECK(outgoing == 0);
    pthread_mutex_lock(&g_lock);
    s->closes[reason]++;
    pthread_mutex_unlock(&g_lock);
}

static void server_start(struct server *s)
{
    memset(s, 0, sizeof *s);
    struct kivfs_reactor_callbacks cb = {
        .on_packet = srv_packet, .on_timer = srv_timer, .on_close = srv_close, .user = s
    };
    struct kivfs_reactor_config cfg = { .bind_addr = "127.0.0.1", .backlog = 16, .max_conns = SRV_SLOTS };
    int rc = KIVFS_EIO;
    for (int i = 0; i < 5 && rc != KIVFS_OK; i++) {
        s->port = cfg.port = pick_port();
        rc = kivfs_reactor_create(&s->r, &cfg, &cb);
    }
    CHECK_RC(rc, KIVFS_OK);
    pthread_create(&s->th, NULL, run_thread, s->r);
}

static void server_stop(struct server *s)
{
    kivfs_reactor_stop(s->r);
    pthread_join(s->th, NULL);
    kivfs_reactor_destroy(s->r);
    for (int i = 0; i < SRV_SLOTS; i++)
        kivfs_pkt_free(&s->slow[i]);
}

/* ------------------------------------------------------------------ */
/* Klient – log událostí                                               */
/* ------------------------------------------------------------------ */

enum ev_kind { EV_CONNECT, EV_RESPONSE, EV_TIMER, EV_CLOSE };

struct rec {
    enum ev_kind  kind;
    kivfs_conn_id id;
    uint64_t      tag;
    int           err;
    int           outgoing;
    int           reason;
    int16_t       status;
    uint32_t      first_u32;   /* 2. záznam odpovědi (po ref), pokud je u32 */
    long long     at_ms;
};

#define MAX_REC 512

struct client {
    struct kivfs_reactor *r;
    pthread_t             th;
    int                   running;
    pthread_mutex_t       lock;
    pthread_cond_t        cond;
    struct rec            recs[MAX_REC];
    int                   n;
};

static void add_rec(struct client *c, struct rec rc)
{
    rc.at_ms = now_ms();
    pthread_mutex_lock(&c->lock);
    if (c->n < MAX_REC)
        c->recs[c->n++] = rc;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
}

static void cl_connect(void *u, kivfs_conn_id id)
{
    add_rec(u, (struct rec){ .kind = EV_CONNECT, .id = id });
}

static void cl_response(void *u, kivfs_conn_id id, uint64_t tag, int err, struct kivfs_packet *resp)
{
    struct rec r = { .kind = EV_RESPONSE, .id = id, .tag = tag, .err = err };
    CHECK((err == KIVFS_OK) == (resp != NULL));
    if (resp != NULL) {
        struct kivfs_pkt_reader rd;
        uint64_t ref;
        r.status = resp->header.return_value;
        kivfs_pkt_reader_init(&rd, resp);
        kivfs_pkt_read_u64(&rd, &ref);
        kivfs_pkt_read_u32(&rd, &r.first_u32);
        kivfs_pkt_free(resp);
    }
    add_rec(u, r);
}

static void cl_timer(void *u, uint64_t tag)
{
    add_rec(u, (struct rec){ .kind = EV_TIMER, .tag = tag });
}

static void cl_close(void *u, kivfs_conn_id id, int outgoing, enum kivfs_close_reason reason, int err)
{
    (void)err;
    add_rec(u, (struct rec){ .kind = EV_CLOSE, .id = id, .outgoing = outgoing, .reason = reason });
}

static void cl_packet(void *u, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    (void)u; (void)id;
    kivfs_pkt_free(pkt);
}

static void client_start(struct client *c, uint32_t max_out, uint32_t max_pending)
{
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);
    struct kivfs_reactor_callbacks cb = {
        .on_packet = cl_packet, .on_connect = cl_connect, .on_response = cl_response,
        .on_timer = cl_timer, .on_close = cl_close, .user = c
    };
    struct kivfs_reactor_config cfg = {
        .bind_addr = "127.0.0.1", .backlog = 4, .max_conns = 4,
        .max_out_conns = max_out, .max_pending = max_pending, .connect_timeout_ms = 1000
    };
    int rc = KIVFS_EIO;
    for (int i = 0; i < 5 && rc != KIVFS_OK; i++) {
        cfg.port = pick_port();
        rc = kivfs_reactor_create(&c->r, &cfg, &cb);
    }
    CHECK_RC(rc, KIVFS_OK);
    pthread_create(&c->th, NULL, run_thread, c->r);
    c->running = 1;
}

static void client_stop(struct client *c)
{
    if (c->running) {
        kivfs_reactor_stop(c->r);
        pthread_join(c->th, NULL);
        c->running = 0;
    }
    kivfs_reactor_destroy(c->r);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
}

/* Index první události (od from) daného druhu a tagu/id; čeká max timeout_ms.
 * tag/id = UINT64_MAX = libovolný. -1 = nedočkali jsme se. */
#define ANY UINT64_MAX
static int wait_rec(struct client *c, enum ev_kind kind, uint64_t tag, uint64_t id, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    pthread_mutex_lock(&c->lock);
    for (;;) {
        for (int i = 0; i < c->n; i++) {
            struct rec *r = &c->recs[i];
            if (r->kind == kind && (tag == ANY || r->tag == tag) && (id == ANY || r->id == id)) {
                pthread_mutex_unlock(&c->lock);
                return i;
            }
        }
        long long left = deadline - now_ms();
        if (left <= 0)
            break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += left / 1000;
        ts.tv_nsec += (left % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&c->cond, &c->lock, &ts);
    }
    pthread_mutex_unlock(&c->lock);
    return -1;
}

static struct rec get_rec(struct client *c, int i)
{
    struct rec r;
    memset(&r, 0, sizeof r);
    if (i < 0)
        return r;
    pthread_mutex_lock(&c->lock);
    r = c->recs[i];
    pthread_mutex_unlock(&c->lock);
    return r;
}

static int count_rec(struct client *c, enum ev_kind kind, uint64_t tag)
{
    int n = 0;
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < c->n; i++)
        if (c->recs[i].kind == kind && (tag == ANY || c->recs[i].tag == tag))
            n++;
    pthread_mutex_unlock(&c->lock);
    return n;
}

static int do_connect(struct client *c, uint16_t port, kivfs_conn_id *id)
{
    struct sockaddr_storage a;
    socklen_t alen;
    int rc = kivfs_sock_resolve("127.0.0.1", port, &a, &alen);
    if (rc != KIVFS_OK)
        return rc;
    return kivfs_reactor_connect(c->r, (struct sockaddr *)&a, alen, id);
}

static int do_request(struct client *c, kivfs_conn_id id, uint16_t op, uint64_t tag, int timeout_ms)
{
    static uint64_t ts = 1000;
    struct kivfs_packet p;
    kivfs_pkt_init(&p, op, __sync_add_and_fetch(&ts, 1));
    kivfs_pkt_add_u32(&p, (uint32_t)tag);
    return kivfs_reactor_request(c->r, id, &p, tag, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Testy                                                               */
/* ------------------------------------------------------------------ */

/* Požadavky zařazené hned po connect (před navázáním) odejdou po navázání
 * v pořadí a každý dostane právě jednu odpověď s vlastním tagem. */
static void test_connect_and_echo(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    CHECK(id != KIVFS_CONN_ID_INVALID);
    for (uint64_t t = 1; t <= 5; t++)
        CHECK_RC(do_request(&c, id, OP_ECHO, t, 2000), KIVFS_OK);

    int ic = wait_rec(&c, EV_CONNECT, ANY, id, 2000);
    CHECK(ic >= 0);
    int prev = ic;
    for (uint64_t t = 1; t <= 5; t++) {
        int i = wait_rec(&c, EV_RESPONSE, t, id, 2000);
        struct rec r = get_rec(&c, i);
        CHECK(i > prev);                      /* po connectu a v pořadí */
        CHECK(r.err == KIVFS_OK && r.status == KIVFS_STATUS_OK);
        CHECK(r.first_u32 == (uint32_t)t);    /* echo vrátilo náš záznam */
        prev = i;
    }
    CHECK(count_rec(&c, EV_RESPONSE, ANY) == 5);

    kivfs_reactor_close_conn(c.r, id);
    int ix = wait_rec(&c, EV_CLOSE, ANY, id, 2000);
    struct rec r = get_rec(&c, ix);
    CHECK(r.outgoing == 1 && r.reason == KIVFS_CLOSE_LOCAL);
    client_stop(&c);
}

/* Nikdo neposlouchá -> on_close(CONNECT); zařazený požadavek dostane
 * ECONNLOST dřív než on_close. */
static void test_connect_refused(void)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, pick_port(), &id), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_ECHO, 1, 2000), KIVFS_OK);

    int ix = wait_rec(&c, EV_CLOSE, ANY, id, 3000);
    int ir = wait_rec(&c, EV_RESPONSE, 1, ANY, 3000);
    struct rec rx = get_rec(&c, ix), rr = get_rec(&c, ir);
    CHECK(ix >= 0 && rx.reason == KIVFS_CLOSE_CONNECT && rx.outgoing == 1);
    CHECK(ir >= 0 && rr.err == KIVFS_ECONNLOST);
    CHECK(count_rec(&c, EV_CONNECT, ANY) == 0);
    CHECK(count_rec(&c, EV_RESPONSE, 1) == 1);
    client_stop(&c);
}

/* Rozpracovaný požadavek nedostane odpověď -> ETIMEOUT a zavření
 * spojení (RESP_TIMEOUT); požadavek za ním ECONNLOST, oba před on_close. */
static void test_inflight_timeout(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    long long t0 = now_ms();
    CHECK_RC(do_request(&c, id, OP_NOREPLY, 1, 200), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_ECHO, 2, 5000), KIVFS_OK);

    int i1 = wait_rec(&c, EV_RESPONSE, 1, id, 2000);
    int i2 = wait_rec(&c, EV_RESPONSE, 2, id, 2000);
    int ix = wait_rec(&c, EV_CLOSE, ANY, id, 2000);
    struct rec r1 = get_rec(&c, i1), r2 = get_rec(&c, i2), rx = get_rec(&c, ix);
    CHECK(r1.err == KIVFS_ETIMEOUT);
    CHECK(r1.at_ms - t0 >= 200 && r1.at_ms - t0 < 1000);
    CHECK(r2.err == KIVFS_ECONNLOST);
    CHECK(rx.reason == KIVFS_CLOSE_RESP_TIMEOUT);
    CHECK(i1 < ix && i2 < ix);
    client_stop(&c);
}

/* Požadavek čekající ve frontě za pomalým se při timeoutu jen vyřadí;
 * spojení žije a pomalý požadavek doběhne. */
static void test_pending_timeout(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_SLOW, 1, 3000), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_ECHO, 2, 100), KIVFS_OK);

    int i2 = wait_rec(&c, EV_RESPONSE, 2, id, 2000);
    int i1 = wait_rec(&c, EV_RESPONSE, 1, id, 2000);
    CHECK(get_rec(&c, i2).err == KIVFS_ETIMEOUT);
    CHECK(get_rec(&c, i1).err == KIVFS_OK);
    CHECK(i2 < i1);
    CHECK(count_rec(&c, EV_CLOSE, ANY) == 0);

    /* spojení je dál použitelné */
    CHECK_RC(do_request(&c, id, OP_ECHO, 3, 2000), KIVFS_OK);
    CHECK(get_rec(&c, wait_rec(&c, EV_RESPONSE, 3, id, 2000)).err == KIVFS_OK);
    client_stop(&c);
}

/* max_pending = 2: třetí požadavek dostane EAGAIN, první dva projdou. */
static void test_max_pending(struct server *s)
{
    struct client c;
    client_start(&c, 0, 2);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    for (uint64_t t = 1; t <= 3; t++)
        CHECK_RC(do_request(&c, id, OP_SLOW, t, 3000), KIVFS_OK);
    CHECK(get_rec(&c, wait_rec(&c, EV_RESPONSE, 3, id, 1000)).err == KIVFS_EAGAIN);
    CHECK(get_rec(&c, wait_rec(&c, EV_RESPONSE, 1, id, 2000)).err == KIVFS_OK);
    CHECK(get_rec(&c, wait_rec(&c, EV_RESPONSE, 2, id, 2000)).err == KIVFS_OK);
    client_stop(&c);
}

/* Odpověď se špatným ref = porušení protokolu -> zavřít, ECONNLOST. */
static void test_bad_ref(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_BADREF, 1, 2000), KIVFS_OK);
    int ir = wait_rec(&c, EV_RESPONSE, 1, id, 2000);
    int ix = wait_rec(&c, EV_CLOSE, ANY, id, 2000);
    CHECK(get_rec(&c, ir).err == KIVFS_ECONNLOST);
    CHECK(get_rec(&c, ix).reason == KIVFS_CLOSE_PROTOCOL);
    CHECK(ir < ix);
    client_stop(&c);
}

/* Protistrana zavře spojení uprostřed požadavku. */
static void test_peer_drops(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_DROP, 1, 2000), KIVFS_OK);
    int ir = wait_rec(&c, EV_RESPONSE, 1, id, 2000);
    int ix = wait_rec(&c, EV_CLOSE, ANY, id, 2000);
    CHECK(get_rec(&c, ir).err == KIVFS_ECONNLOST);
    CHECK(get_rec(&c, ix).reason == KIVFS_CLOSE_PEER);
    CHECK(ir >= 0 && ir < ix);
    client_stop(&c);
}

/* close_conn na odchozím spojení: hned, i s rozpracovanými požadavky. */
static void test_local_close(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    CHECK(wait_rec(&c, EV_CONNECT, ANY, id, 2000) >= 0);
    CHECK_RC(do_request(&c, id, OP_NOREPLY, 1, 5000), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_ECHO, 2, 5000), KIVFS_OK);
    sleep_ms(50);
    kivfs_reactor_close_conn(c.r, id);
    int i1 = wait_rec(&c, EV_RESPONSE, 1, id, 1000);
    int i2 = wait_rec(&c, EV_RESPONSE, 2, id, 1000);
    int ix = wait_rec(&c, EV_CLOSE, ANY, id, 1000);
    CHECK(get_rec(&c, i1).err == KIVFS_ECONNLOST && get_rec(&c, i2).err == KIVFS_ECONNLOST);
    CHECK(get_rec(&c, ix).reason == KIVFS_CLOSE_LOCAL);
    CHECK(i1 < ix && i2 < ix);

    /* požadavek na už mrtvé id: ECONNLOST, ne ticho */
    CHECK_RC(do_request(&c, id, OP_ECHO, 3, 1000), KIVFS_OK);
    CHECK(get_rec(&c, wait_rec(&c, EV_RESPONSE, 3, ANY, 1000)).err == KIVFS_ECONNLOST);
    client_stop(&c);
}

/* Limit max_out_conns a znovupoužití slotu s novou generací. */
static void test_out_limit(struct server *s)
{
    struct client c;
    client_start(&c, 1, 0);
    kivfs_conn_id a, b;
    CHECK_RC(do_connect(&c, s->port, &a), KIVFS_OK);
    CHECK_RC(do_connect(&c, s->port, &b), KIVFS_EAGAIN);
    CHECK(b == KIVFS_CONN_ID_INVALID);
    CHECK(wait_rec(&c, EV_CONNECT, ANY, a, 2000) >= 0);
    kivfs_reactor_close_conn(c.r, a);
    CHECK(wait_rec(&c, EV_CLOSE, ANY, a, 2000) >= 0);
    CHECK_RC(do_connect(&c, s->port, &b), KIVFS_OK);
    CHECK((uint32_t)a == (uint32_t)b && a != b);    /* stejný slot, jiná generace */
    CHECK_RC(do_request(&c, b, OP_ECHO, 1, 2000), KIVFS_OK);
    CHECK(get_rec(&c, wait_rec(&c, EV_RESPONSE, 1, b, 2000)).err == KIVFS_OK);
    client_stop(&c);
}

/* Neplatné argumenty a požadavek na příchozí / neexistující spojení. */
static void test_invalid(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);

    struct kivfs_packet p, req;
    kivfs_pkt_init(&req, OP_ECHO, 1);
    CHECK_RC(kivfs_pkt_init_response(&p, &req, 1, 0), KIVFS_OK);
    CHECK_RC(kivfs_reactor_request(c.r, id, &p, 9, 1000), KIVFS_EINVAL);  /* odpověď */
    CHECK(p.data == NULL);                    /* paket převzat i při chybě */
    CHECK_RC(do_request(&c, id, OP_ECHO, 9, 0), KIVFS_EINVAL);
    CHECK_RC(kivfs_reactor_timer(c.r, -1, 1), KIVFS_EINVAL);
    CHECK_RC(kivfs_reactor_connect(c.r, NULL, 0, &id), KIVFS_EINVAL);

    /* id, které nikdy neplatilo -> ECONNLOST přes callback */
    CHECK_RC(do_request(&c, ((uint64_t)77 << 32) | 1, OP_ECHO, 10, 1000), KIVFS_OK);
    CHECK(get_rec(&c, wait_rec(&c, EV_RESPONSE, 10, ANY, 1000)).err == KIVFS_ECONNLOST);
    sleep_ms(50);
    CHECK(count_rec(&c, EV_RESPONSE, 9) == 0);
    client_stop(&c);
}

/* Časovače: pořadí podle deadline, nikdy dřív než delay. */
static void test_timers(void)
{
    struct client c;
    client_start(&c, 0, 0);
    long long t0 = now_ms();
    CHECK_RC(kivfs_reactor_timer(c.r, 150, 3), KIVFS_OK);
    CHECK_RC(kivfs_reactor_timer(c.r, 50, 1), KIVFS_OK);
    CHECK_RC(kivfs_reactor_timer(c.r, 100, 2), KIVFS_OK);
    CHECK_RC(kivfs_reactor_timer(c.r, 0, 0), KIVFS_OK);
    int i3 = wait_rec(&c, EV_TIMER, 3, ANY, 2000);
    int i0 = wait_rec(&c, EV_TIMER, 0, ANY, 10);
    int i1 = wait_rec(&c, EV_TIMER, 1, ANY, 10);
    int i2 = wait_rec(&c, EV_TIMER, 2, ANY, 10);
    CHECK(i0 >= 0 && i0 < i1 && i1 < i2 && i2 < i3);
    CHECK(get_rec(&c, i1).at_ms - t0 >= 50);
    CHECK(get_rec(&c, i3).at_ms - t0 >= 150 && get_rec(&c, i3).at_ms - t0 < 1000);
    client_stop(&c);
}

/* stop: rozpracovaný požadavek ESHUTDOWN, spojení on_close(SHUTDOWN)
 * až po něm, časovač bez callbacku; po run vrací API KIVFS_EIO. */
static void test_shutdown(struct server *s)
{
    struct client c;
    client_start(&c, 0, 0);
    kivfs_conn_id id;
    CHECK_RC(do_connect(&c, s->port, &id), KIVFS_OK);
    CHECK_RC(do_request(&c, id, OP_NOREPLY, 1, 10000), KIVFS_OK);
    CHECK_RC(kivfs_reactor_timer(c.r, 10000, 5), KIVFS_OK);
    CHECK(wait_rec(&c, EV_CONNECT, ANY, id, 2000) >= 0);
    sleep_ms(50);

    kivfs_reactor_stop(c.r);
    pthread_join(c.th, NULL);
    c.running = 0;

    int ir = wait_rec(&c, EV_RESPONSE, 1, id, 0);
    int ix = wait_rec(&c, EV_CLOSE, ANY, id, 0);
    CHECK(get_rec(&c, ir).err == KIVFS_ESHUTDOWN);
    CHECK(get_rec(&c, ix).reason == KIVFS_CLOSE_SHUTDOWN);
    CHECK(ir >= 0 && ir < ix);
    CHECK(count_rec(&c, EV_TIMER, ANY) == 0);

    kivfs_conn_id id2;
    CHECK_RC(do_request(&c, id, OP_ECHO, 2, 1000), KIVFS_EIO);
    CHECK_RC(do_connect(&c, s->port, &id2), KIVFS_EIO);
    CHECK_RC(kivfs_reactor_timer(c.r, 1, 1), KIVFS_EIO);
    client_stop(&c);
}

/* Paket s KIVFS_RESPONSE_FLAG na PŘÍCHOZÍM spojení = porušení protokolu. */
static void test_response_on_incoming(struct server *s)
{
    int before;
    pthread_mutex_lock(&g_lock);
    before = s->closes[KIVFS_CLOSE_PROTOCOL];
    pthread_mutex_unlock(&g_lock);

    int fd = kivfs_sock_connect("127.0.0.1", s->port, 1000);
    CHECK(fd >= 0);
    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, OP_ECHO, 1);
    kivfs_pkt_init_response(&resp, &req, 1, 0);
    uint8_t hdr[KIVFS_HEADER_WIRE_SIZE];
    kivfs_proto_header_encode(&resp.header, hdr);
    struct iovec iov[2] = { { hdr, sizeof hdr }, { resp.data, resp.header.data_size } };
    CHECK_RC(kivfs_sock_writev_full(fd, iov, 2), KIVFS_OK);
    uint8_t b;
    kivfs_sock_set_timeout(fd, 2000);
    CHECK_RC(kivfs_sock_read_full(fd, &b, 1), KIVFS_ECLOSED);   /* server zavřel */
    kivfs_sock_close(&fd);
    kivfs_pkt_free(&resp);
    sleep_ms(20);
    pthread_mutex_lock(&g_lock);
    int after = s->closes[KIVFS_CLOSE_PROTOCOL];
    pthread_mutex_unlock(&g_lock);
    CHECK(after == before + 1);
}

int main(void)
{
    kivfs_sock_ignore_sigpipe();
    struct server s;
    server_start(&s);

    test_connect_and_echo(&s);
    test_connect_refused();
    test_inflight_timeout(&s);
    test_pending_timeout(&s);
    test_max_pending(&s);
    test_bad_ref(&s);
    test_peer_drops(&s);
    test_local_close(&s);
    test_out_limit(&s);
    test_invalid(&s);
    test_timers();
    test_shutdown(&s);
    test_response_on_incoming(&s);

    server_stop(&s);
    printf("reactor_out: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
