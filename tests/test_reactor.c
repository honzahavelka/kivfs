/*
 * Testy epoll reactoru (jen Linux).
 *
 * Každý test si spustí vlastní "mini server": reactor v samostatném
 * I/O vlákně + fronta (queue.h) + několik workerů, tedy stejné zapojení
 * jako skutečný server. Klienti jsou blokující kanály (channel.h), vadné
 * pakety se posílají jako surové bajty.
 *
 * Operace testovacího serveru (request_code):
 *   OP_ECHO   odpověď = ref + kopie všech záznamů požadavku
 *   OP_SLOW   jako ECHO, ale worker nejdřív 30 ms spí (simulace DB)
 *   OP_CLOSE  odpoví a pak zavře spojení (kivfs_reactor_close_conn)
 *
 * Worker navíc hlídá pravidlo "jeden požadavek na spojení": počítá,
 * kolik požadavků téhož spojení se právě zpracovává; víc než 1 = chyba.
 *
 * Pouštět i pod ThreadSanitizerem a Valgrindem.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "channel.h"
#include "errors.h"
#include "packet.h"
#include "queue.h"
#include "reactor.h"
#include "socket.h"

static atomic_int g_checks;
static atomic_int g_failed;

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
        g_checks++;                                                        \
        if (rc_ != (expected)) {                                           \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s -> %d (%s), expected %s\n",   \
                    __FILE__, __LINE__, #expr, rc_, kivfs_strerror(rc_),   \
                    #expected);                                            \
        }                                                                  \
    } while (0)

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

enum { OP_ECHO = 100, OP_SLOW = 101, OP_CLOSE = 102 };

#define TIMEOUT_MS   3000
#define N_WORKERS    4
#define MAX_SLOTS    64

/* ------------------------------------------------------------------ */
/* Testovací server                                                    */
/* ------------------------------------------------------------------ */

struct job {
    kivfs_conn_id       conn;
    struct kivfs_packet pkt;
};

struct server {
    struct kivfs_reactor *r;
    struct kivfs_queue   *q;
    pthread_t             io_thread;
    pthread_t             workers[N_WORKERS];
    uint16_t              port;
    int                   run_rc;

    /* události z callbacků (I/O vlákno) – čte je test */
    pthread_mutex_t lock;
    int             accepts;
    int             packets;
    int             closes[KIVFS_CLOSE_RESP_TIMEOUT + 1];
    kivfs_conn_id   last_accept_id;
    kivfs_conn_id   last_close_id;

    /* pravidlo "jeden požadavek na spojení" (pod lock) */
    int             in_flight[MAX_SLOTS];
    int             violations;
};

static void job_free(void *p)
{
    kivfs_pkt_free(&((struct job *)p)->pkt);
}

static void cb_accept(void *user, kivfs_conn_id id, const char *peer)
{
    struct server *s = user;
    (void)peer;
    pthread_mutex_lock(&s->lock);
    s->accepts++;
    s->last_accept_id = id;
    pthread_mutex_unlock(&s->lock);
}

static void cb_packet(void *user, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    struct server *s = user;
    struct job j = { id, *pkt };

    pthread_mutex_lock(&s->lock);
    s->packets++;
    pthread_mutex_unlock(&s->lock);

    if (kivfs_queue_try_push(s->q, &j) != KIVFS_OK) {
        struct kivfs_packet resp;
        if (kivfs_pkt_init_response(&resp, &j.pkt, 0, KIVFS_STATUS_BUSY) == KIVFS_OK)
            kivfs_reactor_send(s->r, id, &resp);
        kivfs_pkt_free(&j.pkt);
    }
}

static void cb_close(void *user, kivfs_conn_id id, int outgoing,
                     enum kivfs_close_reason reason, int err)
{
    struct server *s = user;
    (void)err;
    (void)outgoing;
    pthread_mutex_lock(&s->lock);
    s->closes[reason]++;
    s->last_close_id = id;
    pthread_mutex_unlock(&s->lock);
}

/* Odpověď = ref + kopie všech záznamů požadavku. */
static int build_echo(struct kivfs_packet *resp, const struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    const uint8_t *b;
    uint32_t len;
    int rc = kivfs_pkt_init_response(resp, req, 0, KIVFS_STATUS_OK);
    if (rc != KIVFS_OK)
        return rc;
    kivfs_pkt_reader_init(&rd, req);
    while ((rc = kivfs_pkt_read_bytes(&rd, &b, &len)) == KIVFS_OK)
        if ((rc = kivfs_pkt_add_bytes(resp, b, len)) != KIVFS_OK)
            break;
    return rc == KIVFS_EEND ? KIVFS_OK : rc;
}

static void *worker_main(void *arg)
{
    struct server *s = arg;
    struct job j;

    while (kivfs_queue_pop(s->q, &j) == KIVFS_OK) {
        uint32_t slot = (uint32_t)j.conn % MAX_SLOTS;
        pthread_mutex_lock(&s->lock);
        if (s->in_flight[slot]++ != 0)
            s->violations++;
        pthread_mutex_unlock(&s->lock);

        uint16_t op = j.pkt.header.request_code;
        if (op == OP_SLOW)
            sleep_ms(30);

        struct kivfs_packet resp;
        int rc = build_echo(&resp, &j.pkt);

        /* Snížit PŘED odesláním – po odeslání odpovědi už smí přijít
         * další požadavek téhož spojení. */
        pthread_mutex_lock(&s->lock);
        s->in_flight[slot]--;
        pthread_mutex_unlock(&s->lock);

        if (rc == KIVFS_OK)
            kivfs_reactor_send(s->r, j.conn, &resp);
        if (op == OP_CLOSE)
            kivfs_reactor_close_conn(s->r, j.conn);
        kivfs_pkt_free(&j.pkt);
    }
    return NULL;
}

static void *io_main(void *arg)
{
    struct server *s = arg;
    s->run_rc = kivfs_reactor_run(s->r);
    return NULL;
}

/* Volný port: jádro přidělí port 0, zjistíme ho a socket zavřeme.
 * (Malé okno, kdy ho může vzít někdo jiný – pro testy to stačí.) */
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

static int server_start(struct server *s, uint32_t max_conns, int rx_timeout_ms)
{
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->lock, NULL);

    struct kivfs_reactor_callbacks cb = {
        .on_accept = cb_accept, .on_packet = cb_packet,
        .on_close = cb_close, .user = s
    };
    struct kivfs_reactor_config cfg = {
        .bind_addr = "127.0.0.1", .backlog = 64,
        .max_conns = max_conns, .rx_timeout_ms = rx_timeout_ms
    };

    int rc = KIVFS_EIO;
    for (int attempt = 0; attempt < 5 && rc != KIVFS_OK; attempt++) {
        s->port = cfg.port = pick_port();
        rc = kivfs_reactor_create(&s->r, &cfg, &cb);
    }
    if (rc != KIVFS_OK)
        return rc;

    rc = kivfs_queue_create(&s->q, max_conns, sizeof(struct job), job_free);
    if (rc != KIVFS_OK)
        return rc;
    for (int i = 0; i < N_WORKERS; i++)
        pthread_create(&s->workers[i], NULL, worker_main, s);
    pthread_create(&s->io_thread, NULL, io_main, s);
    return KIVFS_OK;
}

/* Pořadí podle reactor.h: stop -> počkat na run -> zastavit workery -> destroy. */
static void server_stop(struct server *s)
{
    kivfs_reactor_stop(s->r);
    pthread_join(s->io_thread, NULL);
    kivfs_queue_shutdown(s->q);
    for (int i = 0; i < N_WORKERS; i++)
        pthread_join(s->workers[i], NULL);
    kivfs_queue_destroy(s->q);
    kivfs_reactor_destroy(s->r);
    pthread_mutex_destroy(&s->lock);
}

/* Počká, až čítač dosáhne aspoň want (callbacky běží v jiném vlákně). */
static int wait_count(struct server *s, const int *counter, int want)
{
    for (int i = 0; i < TIMEOUT_MS / 5; i++) {
        pthread_mutex_lock(&s->lock);
        int v = *counter;
        pthread_mutex_unlock(&s->lock);
        if (v >= want)
            return 1;
        sleep_ms(5);
    }
    return 0;
}

static int get_count(struct server *s, const int *counter)
{
    pthread_mutex_lock(&s->lock);
    int v = *counter;
    pthread_mutex_unlock(&s->lock);
    return v;
}

static int client_open(struct server *s, struct kivfs_chan *ch)
{
    kivfs_chan_init(ch);
    return kivfs_chan_open(ch, "127.0.0.1", s->port, TIMEOUT_MS);
}

/* Surové spojení (blokující fd) pro posílání vadných dat. */
static int raw_open(struct server *s)
{
    int fd = kivfs_sock_connect("127.0.0.1", s->port, TIMEOUT_MS);
    if (fd >= 0)
        kivfs_sock_set_timeout(fd, TIMEOUT_MS);
    return fd;
}

static void raw_write(int fd, const void *buf, size_t len)
{
    struct iovec iov = { (void *)buf, len };
    CHECK_RC(kivfs_sock_writev_full(fd, &iov, 1), KIVFS_OK);
}

/* Pošle požadavek s jedním řetězcovým záznamem a ověří echo. */
static void call_echo(struct kivfs_chan *ch, uint16_t op, uint64_t ts, const char *str)
{
    struct kivfs_packet req, resp;
    struct kivfs_pkt_reader rd;
    char buf[256];
    uint64_t ref;

    kivfs_pkt_init(&req, op, ts);
    kivfs_pkt_add_string(&req, str);
    CHECK_RC(kivfs_chan_call(ch, &req, &resp), KIVFS_OK);
    kivfs_pkt_reader_init(&rd, &resp);
    CHECK_RC(kivfs_pkt_read_u64(&rd, &ref), KIVFS_OK);
    CHECK(ref == ts);
    CHECK_RC(kivfs_pkt_read_string(&rd, buf, sizeof buf), KIVFS_OK);
    CHECK(strcmp(buf, str) == 0);
    CHECK(kivfs_pkt_reader_at_end(&rd));
    kivfs_pkt_free(&req);
    kivfs_pkt_free(&resp);
}

/* ------------------------------------------------------------------ */
/* Testy                                                               */
/* ------------------------------------------------------------------ */

static void test_invalid_args(void)
{
    struct kivfs_reactor *r = (struct kivfs_reactor *)1;
    struct kivfs_reactor_callbacks cb = { 0 };
    struct kivfs_reactor_config cfg = { .bind_addr = "127.0.0.1", .backlog = 4, .max_conns = 4 };

    CHECK_RC(kivfs_reactor_create(&r, &cfg, &cb), KIVFS_EINVAL);   /* chybí on_packet */
    CHECK(r == NULL);
    cb.on_packet = cb_packet;
    cfg.max_conns = 0;
    CHECK_RC(kivfs_reactor_create(&r, &cfg, &cb), KIVFS_EINVAL);
    kivfs_reactor_destroy(NULL);                                     /* nesmí spadnout */
    kivfs_reactor_stop(NULL);
}

static void test_echo_and_peer_close(void)
{
    struct server s;
    struct kivfs_chan ch;
    CHECK_RC(server_start(&s, 8, 0), KIVFS_OK);

    CHECK_RC(client_open(&s, &ch), KIVFS_OK);
    for (uint64_t ts = 1; ts <= 10; ts++)
        call_echo(&ch, OP_ECHO, ts, "/kivfs/a/b");

    /* prázdný paket (data_size = 0) – odpověď nese jen ref */
    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, OP_ECHO, 99);
    CHECK_RC(kivfs_chan_call(&ch, &req, &resp), KIVFS_OK);
    CHECK(resp.header.data_size == 12);    /* [u32 len=8][u64 ref] */
    kivfs_pkt_free(&resp);

    CHECK(wait_count(&s, &s.accepts, 1));
    kivfs_chan_close(&ch);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_PEER], 1));
    CHECK(get_count(&s, &s.packets) == 11);
    server_stop(&s);
}

/* Klient pošle 5 požadavků najednou (nečeká na odpovědi). Reactor je musí
 * předávat po jednom, takže odpovědi přijdou v pořadí a na spojení nikdy
 * nepracují dva workery zároveň. */
static void test_pipelined_one_at_a_time(void)
{
    struct server s;
    struct kivfs_chan ch;
    CHECK_RC(server_start(&s, 8, 0), KIVFS_OK);
    CHECK_RC(client_open(&s, &ch), KIVFS_OK);

    for (uint64_t ts = 1; ts <= 5; ts++) {
        struct kivfs_packet req;
        kivfs_pkt_init(&req, OP_SLOW, ts);
        kivfs_pkt_add_u64(&req, ts * 1000);
        CHECK_RC(kivfs_chan_send(&ch, &req), KIVFS_OK);
        kivfs_pkt_free(&req);
    }
    for (uint64_t ts = 1; ts <= 5; ts++) {
        struct kivfs_packet resp;
        uint64_t ref = 0;
        CHECK_RC(kivfs_chan_recv(&ch, &resp), KIVFS_OK);
        CHECK_RC(kivfs_pkt_response_ref(&resp, &ref), KIVFS_OK);
        CHECK(ref == ts);
        kivfs_pkt_free(&resp);
    }
    CHECK(get_count(&s, &s.violations) == 0);

    kivfs_chan_close(&ch);
    server_stop(&s);
}

/* Víc klientů souběžně, každý se svými daty. */
#define MC_CLIENTS 16
#define MC_CALLS   200

struct mc_arg { struct server *s; int idx; };

static void *mc_client(void *p)
{
    struct mc_arg *a = p;
    struct kivfs_chan ch;
    char str[64];
    CHECK_RC(client_open(a->s, &ch), KIVFS_OK);
    for (int i = 0; i < MC_CALLS; i++) {
        snprintf(str, sizeof str, "klient-%d-volani-%d", a->idx, i);
        call_echo(&ch, OP_ECHO, (uint64_t)i + 1, str);
    }
    kivfs_chan_close(&ch);
    return NULL;
}

static void test_many_clients(void)
{
    struct server s;
    pthread_t th[MC_CLIENTS];
    struct mc_arg args[MC_CLIENTS];
    CHECK_RC(server_start(&s, MC_CLIENTS, 1000), KIVFS_OK);

    for (int i = 0; i < MC_CLIENTS; i++) {
        args[i].s = &s;
        args[i].idx = i;
        pthread_create(&th[i], NULL, mc_client, &args[i]);
    }
    for (int i = 0; i < MC_CLIENTS; i++)
        pthread_join(th[i], NULL);

    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_PEER], MC_CLIENTS));
    CHECK(get_count(&s, &s.packets) == MC_CLIENTS * MC_CALLS);
    CHECK(get_count(&s, &s.violations) == 0);
    server_stop(&s);
}

/* 8 MiB tam i zpět: víc než buffer socketu, takže reactor čte po částech
 * a odpověď odesílá postupně přes EPOLLOUT. */
static void test_large_payload(void)
{
    struct server s;
    struct kivfs_chan ch;
    const uint32_t big = 8u * 1024u * 1024u;
    uint8_t *buf = malloc(big);
    CHECK(buf != NULL);
    for (uint32_t i = 0; i < big; i++)
        buf[i] = (uint8_t)(i * 31u + 7u);

    CHECK_RC(server_start(&s, 4, 5000), KIVFS_OK);
    CHECK_RC(client_open(&s, &ch), KIVFS_OK);

    struct kivfs_packet req, resp;
    struct kivfs_pkt_reader rd;
    const uint8_t *data;
    uint32_t len;
    uint64_t ref;
    kivfs_pkt_init(&req, OP_ECHO, 7);
    CHECK_RC(kivfs_pkt_add_bytes(&req, buf, big), KIVFS_OK);
    CHECK_RC(kivfs_chan_call(&ch, &req, &resp), KIVFS_OK);
    kivfs_pkt_reader_init(&rd, &resp);
    CHECK_RC(kivfs_pkt_read_u64(&rd, &ref), KIVFS_OK);
    CHECK_RC(kivfs_pkt_read_bytes(&rd, &data, &len), KIVFS_OK);
    CHECK(len == big && memcmp(data, buf, big) == 0);
    kivfs_pkt_free(&req);
    kivfs_pkt_free(&resp);

    /* spojení je po velkém přenosu dál použitelné */
    call_echo(&ch, OP_ECHO, 8, "po velkem paketu");

    kivfs_chan_close(&ch);
    server_stop(&s);
    free(buf);
}

static void make_header(uint8_t out[KIVFS_HEADER_WIRE_SIZE], uint32_t magic, uint32_t data_size)
{
    struct kivfs_header h = { magic, 1, OP_ECHO, 0, data_size };
    kivfs_proto_header_encode(&h, out);
}

static void test_protocol_errors(void)
{
    struct server s;
    uint8_t hdr[KIVFS_HEADER_WIRE_SIZE];
    uint8_t dummy;
    CHECK_RC(server_start(&s, 8, 0), KIVFS_OK);

    /* špatný magic */
    int fd = raw_open(&s);
    CHECK(fd >= 0);
    make_header(hdr, 0xDEADBEEFu, 0);
    raw_write(fd, hdr, sizeof hdr);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_PROTOCOL], 1));
    CHECK(kivfs_sock_read_full(fd, &dummy, 1) != KIVFS_OK);   /* server zavřel */
    kivfs_sock_close(&fd);

    /* data_size nad KIVFS_MAX_PAYLOAD – nesmí se ani alokovat */
    fd = raw_open(&s);
    make_header(hdr, KIVFS_MAGIC, KIVFS_MAX_PAYLOAD + 1);
    raw_write(fd, hdr, sizeof hdr);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_PROTOCOL], 2));
    kivfs_sock_close(&fd);

    /* konec spojení uprostřed payloadu */
    fd = raw_open(&s);
    make_header(hdr, KIVFS_MAGIC, 100);
    raw_write(fd, hdr, sizeof hdr);
    raw_write(fd, "abc", 3);
    kivfs_sock_close(&fd);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_IO_ERROR], 1));

    /* konec uprostřed hlavičky */
    fd = raw_open(&s);
    raw_write(fd, hdr, 7);
    kivfs_sock_close(&fd);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_IO_ERROR], 2));

    CHECK(get_count(&s, &s.packets) == 0);
    server_stop(&s);
}

/* rx_timeout_ms: neúplný paket se zavře, nečinné spojení MEZI pakety ne. */
static void test_rx_timeout(void)
{
    struct server s;
    struct kivfs_chan idle;
    uint8_t hdr[KIVFS_HEADER_WIRE_SIZE];
    CHECK_RC(server_start(&s, 8, 150), KIVFS_OK);

    CHECK_RC(client_open(&s, &idle), KIVFS_OK);
    call_echo(&idle, OP_ECHO, 1, "pred pauzou");

    int fd = raw_open(&s);
    make_header(hdr, KIVFS_MAGIC, 50);
    raw_write(fd, hdr, 10);                    /* půl hlavičky a dost */
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_TIMEOUT], 1));

    sleep_ms(400);                             /* > 2× timeout */
    CHECK(get_count(&s, &s.closes[KIVFS_CLOSE_TIMEOUT]) == 1);
    call_echo(&idle, OP_ECHO, 2, "po pauze");  /* nečinné spojení žije */

    kivfs_sock_close(&fd);
    kivfs_chan_close(&idle);
    server_stop(&s);
}

/* close_conn po odpovědi: klient odpověď dostane, pak konec spojení. */
static void test_close_conn(void)
{
    struct server s;
    struct kivfs_chan ch;
    struct kivfs_packet resp;
    CHECK_RC(server_start(&s, 8, 0), KIVFS_OK);
    CHECK_RC(client_open(&s, &ch), KIVFS_OK);

    call_echo(&ch, OP_CLOSE, 1, "posledni");
    CHECK_RC(kivfs_chan_recv(&ch, &resp), KIVFS_ECLOSED);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_LOCAL], 1));

    kivfs_chan_close(&ch);
    server_stop(&s);
}

/* Pozdní odpověď na staré conn_id nesmí dorazit novému spojení, které
 * dostalo stejný slot (generace). */
static void test_stale_id(void)
{
    struct server s;
    struct kivfs_chan a, b;
    CHECK_RC(server_start(&s, 1, 0), KIVFS_OK);   /* 1 slot -> jistě se znovu použije */

    CHECK_RC(client_open(&s, &a), KIVFS_OK);
    call_echo(&a, OP_ECHO, 1, "a");
    kivfs_chan_close(&a);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_PEER], 1));
    pthread_mutex_lock(&s.lock);
    kivfs_conn_id old_id = s.last_close_id;
    pthread_mutex_unlock(&s.lock);

    CHECK_RC(client_open(&s, &b), KIVFS_OK);
    CHECK(wait_count(&s, &s.accepts, 2));
    pthread_mutex_lock(&s.lock);
    kivfs_conn_id new_id = s.last_accept_id;
    pthread_mutex_unlock(&s.lock);
    CHECK((uint32_t)old_id == (uint32_t)new_id);   /* stejný slot */
    CHECK(old_id != new_id);                       /* jiná generace */

    /* "zbloudilá" odpověď na staré id – musí se zahodit */
    struct kivfs_packet stray;
    kivfs_pkt_init(&stray, kivfs_proto_response_code(OP_ECHO), 0);
    kivfs_pkt_add_u64(&stray, 12345);
    CHECK_RC(kivfs_reactor_send(s.r, old_id, &stray), KIVFS_OK);
    CHECK(stray.data == NULL);                     /* paket převzat */
    CHECK_RC(kivfs_reactor_send(s.r, KIVFS_CONN_ID_INVALID, &stray), KIVFS_OK);
    kivfs_reactor_close_conn(s.r, old_id);         /* nesmí zavřít b */

    /* kdyby zbloudilá odpověď prošla, chan_call vrátí KIVFS_EUNEXPECTED */
    call_echo(&b, OP_ECHO, 2, "b");
    CHECK(get_count(&s, &s.closes[KIVFS_CLOSE_LOCAL]) == 0);

    kivfs_chan_close(&b);
    server_stop(&s);
}

/* Nad max_conns se spojení hned zavře, ostatní fungují dál. */
static void test_max_conns(void)
{
    struct server s;
    struct kivfs_chan c1, c2, c3;
    struct kivfs_packet resp;
    CHECK_RC(server_start(&s, 2, 0), KIVFS_OK);

    CHECK_RC(client_open(&s, &c1), KIVFS_OK);
    CHECK_RC(client_open(&s, &c2), KIVFS_OK);
    CHECK(wait_count(&s, &s.accepts, 2));

    CHECK_RC(client_open(&s, &c3), KIVFS_OK);      /* TCP handshake projde (backlog) */
    CHECK_RC(kivfs_chan_recv(&c3, &resp), KIVFS_ECLOSED);
    CHECK(get_count(&s, &s.accepts) == 2);

    call_echo(&c1, OP_ECHO, 1, "c1");
    call_echo(&c2, OP_ECHO, 1, "c2");

    /* po uvolnění slotu se zase dá připojit */
    kivfs_chan_close(&c1);
    CHECK(wait_count(&s, &s.closes[KIVFS_CLOSE_PEER], 1));
    kivfs_chan_close(&c3);
    CHECK_RC(client_open(&s, &c3), KIVFS_OK);
    call_echo(&c3, OP_ECHO, 1, "c3");

    kivfs_chan_close(&c2);
    kivfs_chan_close(&c3);
    server_stop(&s);
}

/* Klient zavře spojení, zatímco worker ještě pracuje: pozdní odpověď se
 * zahodí nebo selže zápis, server nespadne a spojení uklidí. */
static void test_peer_gone_during_request(void)
{
    struct server s;
    struct kivfs_chan ch;
    struct kivfs_packet req;
    CHECK_RC(server_start(&s, 4, 0), KIVFS_OK);
    CHECK_RC(client_open(&s, &ch), KIVFS_OK);

    kivfs_pkt_init(&req, OP_SLOW, 1);
    CHECK_RC(kivfs_chan_send(&ch, &req), KIVFS_OK);
    CHECK(wait_count(&s, &s.packets, 1));
    kivfs_chan_close(&ch);

    sleep_ms(100);                                 /* worker odpoví do prázdna */
    int total = 0;
    for (int i = 0; i < TIMEOUT_MS / 5 && total == 0; i++) {
        total = get_count(&s, &s.closes[KIVFS_CLOSE_PEER])
              + get_count(&s, &s.closes[KIVFS_CLOSE_IO_ERROR]);
        if (total == 0)
            sleep_ms(5);
    }
    CHECK(total == 1);

    /* server dál obsluhuje */
    CHECK_RC(client_open(&s, &ch), KIVFS_OK);
    call_echo(&ch, OP_ECHO, 1, "dal");
    kivfs_chan_close(&ch);
    server_stop(&s);
}

/* stop: otevřená spojení dostanou on_close(SHUTDOWN), klienti konec spojení,
 * send po skončení run vrací KIVFS_EIO. */
static void test_shutdown(void)
{
    struct server s;
    struct kivfs_chan a, b;
    struct kivfs_packet resp;
    CHECK_RC(server_start(&s, 4, 0), KIVFS_OK);
    CHECK_RC(client_open(&s, &a), KIVFS_OK);
    CHECK_RC(client_open(&s, &b), KIVFS_OK);
    call_echo(&a, OP_ECHO, 1, "a");
    CHECK(wait_count(&s, &s.accepts, 2));

    kivfs_reactor_stop(s.r);
    pthread_join(s.io_thread, NULL);
    CHECK_RC(s.run_rc, KIVFS_OK);
    CHECK(get_count(&s, &s.closes[KIVFS_CLOSE_SHUTDOWN]) == 2);
    CHECK_RC(kivfs_chan_recv(&a, &resp), KIVFS_ECLOSED);

    struct kivfs_packet p;
    kivfs_pkt_init(&p, OP_ECHO, 1);
    kivfs_pkt_add_u32(&p, 1);
    CHECK_RC(kivfs_reactor_send(s.r, s.last_accept_id, &p), KIVFS_EIO);
    CHECK(p.data == NULL);                         /* převzat i při chybě */
    CHECK_RC(kivfs_reactor_run(s.r), KIVFS_EINVAL); /* run jen jednou */

    /* dokončit úklid bez druhého stop/join */
    kivfs_queue_shutdown(s.q);
    for (int i = 0; i < N_WORKERS; i++)
        pthread_join(s.workers[i], NULL);
    kivfs_queue_destroy(s.q);
    kivfs_reactor_destroy(s.r);
    pthread_mutex_destroy(&s.lock);
    kivfs_chan_close(&a);
    kivfs_chan_close(&b);
}

int main(void)
{
    kivfs_sock_ignore_sigpipe();

    test_invalid_args();
    test_echo_and_peer_close();
    test_pipelined_one_at_a_time();
    test_many_clients();
    test_large_payload();
    test_protocol_errors();
    test_rx_timeout();
    test_close_conn();
    test_stale_id();
    test_max_conns();
    test_peer_gone_during_request();
    test_shutdown();

    printf("reactor: %d checks, %d failed\n", (int)g_checks, (int)g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
