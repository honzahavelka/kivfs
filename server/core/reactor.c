/*
 * KIVFS – epoll reactor (implementace), viz reactor.h
 * ===================================================
 *
 * Vnitřní uspořádání:
 *
 *   conns[total]       tabulka slotů spojení. Sahá na ni JEN I/O vlákno.
 *                      Sloty 0..max_conns-1 jsou příchozí, max_conns..total-1
 *                      odchozí (oddělené limity, viz reactor.h).
 *   in_free            volné příchozí sloty (zásobník, jen I/O vlákno).
 *   out_free           volné odchozí sloty (zásobník pod mb_lock):
 *                      kivfs_reactor_connect si slot rezervuje v cizím
 *                      vlákně, aby mohl conn_id vrátit hned.
 *   mailbox            jednosměrný seznam zpráv od workerů (SEND / CLOSE /
 *                      CONNECT / REQUEST / TIMER), chráněný mutexem;
 *                      I/O vlákno ho celý "ukradne" a zpracuje bez zámku.
 *   timers             seřazený seznam časovačů (podle deadline). Uzel =
 *                      zpráva z mailboxu, takže se v I/O vlákně nealokuje.
 *                      Vkládání O(n) – časovačů jsou stovky, ne miliony.
 *   wake_fd            eventfd v epollu; zápis do něj probudí epoll_wait.
 *
 * Odchozí spojení: požadavky čekají v pend (FIFO). Jeden je "cur" – zapisuje
 * se nebo čeká na odpověď; jeho tag/timestamp/kód/deadline jsou v conn,
 * protože samotná zpráva se po zápisu uvolní jako u odpovědí.
 *
 * Jedna struktura struct out_msg slouží jako zpráva v mailboxu i jako
 * položka fronty k odeslání daného spojení – I/O vlákno ji jen přepojí,
 * při odesílání se tedy v I/O vlákně nic nealokuje.
 *
 * Token v epoll_event.data.u64 je celé conn_id (generace + slot). Když se
 * během jedné dávky událostí spojení zavře a jeho slot hned obsadí nové
 * spojení (accept ve stejné dávce), stará událost má jinou generaci
 * a zahodí se. Generace 0 není platná, proto tokeny listen socketu
 * a eventfd mají generaci 0 a nemůžou se s žádným spojením splést.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/reactor.h"
#include "proto/errors.h"
#include "net/socket.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* Kolik událostí vybrat jedním epoll_wait. */
#define MAX_EVENTS               64

/* Jak dlouho nepřijímat nová spojení po chybě accept (typicky EMFILE –
 * došly file deskriptory). Bez pauzy by level-triggered epoll hlásil
 * čekající spojení pořád dokola a smyčka by se točila na 100 % CPU. */
#define ACCEPT_BACKOFF_MS        100

/* Výchozí hodnoty konfigurace (0 v kivfs_reactor_config). */
#define DEFAULT_MAX_OUT_CONNS       64
#define DEFAULT_MAX_PENDING         16
#define DEFAULT_CONNECT_TIMEOUT_MS  5000

#define TOKEN_LISTEN             ((uint64_t)1)   /* generace 0, slot 1 */
#define TOKEN_WAKE               ((uint64_t)2)   /* generace 0, slot 2 */

#define LOG(...)  fprintf(stderr, "[reactor] " __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Struktury                                                           */
/* ------------------------------------------------------------------ */

enum msg_kind { MSG_SEND, MSG_CLOSE, MSG_CONNECT, MSG_REQUEST, MSG_TIMER };

struct out_msg {
    struct out_msg     *next;
    enum msg_kind       kind;
    kivfs_conn_id       id;
    /* MSG_SEND, MSG_REQUEST: */
    uint8_t             hdr[KIVFS_HEADER_WIRE_SIZE];  /* zakódovaná hlavička */
    struct kivfs_packet pkt;
    size_t              off;      /* kolik bajtů (hlavička + payload) už odešlo */
    /* MSG_REQUEST, MSG_TIMER: */
    uint64_t            tag;
    long long           deadline_ms;
    /* MSG_CONNECT: */
    struct sockaddr_storage addr;
    socklen_t           addrlen;
};

struct conn {
    int       fd;
    uint32_t  gen;                /* generace slotu, nikdy 0 */
    uint8_t   active;
    uint8_t   awaiting;           /* doručen požadavek, čeká se na odpověď */
    uint8_t   closing;            /* close_conn: zavřít po odeslání fronty */
    uint8_t   outgoing;           /* založeno přes kivfs_reactor_connect */
    uint8_t   connecting;         /* odchozí: connect ještě neskončil */
    uint32_t  ev_mask;            /* události aktuálně registrované v epollu */

    /* příjem rozpracovaného paketu */
    uint8_t   hdr[KIVFS_HEADER_WIRE_SIZE];
    uint32_t  hdr_got;            /* 0 = žádný paket není rozpracovaný */
    struct kivfs_packet rx;       /* platný až po dekódování hlavičky */
    uint32_t  data_got;
    long long rx_start_ms;        /* čas 1. bajtu paketu (rx_timeout_ms) */

    /* fronta k odeslání */
    struct out_msg *tx_head, *tx_tail;

    /* jen odchozí spojení */
    long long       connect_deadline_ms;
    struct out_msg *pend_head, *pend_tail;  /* požadavky čekající na odeslání */
    uint32_t        pend_count;
    uint8_t         cur_active;   /* požadavek se zapisuje / čeká na odpověď */
    uint16_t        cur_code;
    uint64_t        cur_ts;       /* header.timestamp = očekávaný ref */
    uint64_t        cur_tag;
    long long       cur_deadline_ms;
};

struct kivfs_reactor {
    struct kivfs_reactor_config    cfg;
    struct kivfs_reactor_callbacks cb;

    int listen_fd;
    int epoll_fd;
    int wake_fd;

    struct conn *conns;
    uint32_t     total;           /* max_conns + max_out_conns */
    uint32_t    *in_free;
    uint32_t     in_free_top;     /* počet volných příchozích slotů */
    uint32_t    *out_free;        /* pod mb_lock */
    uint32_t     out_free_top;
    uint32_t     out_live;        /* aktivní odchozí spojení (I/O vlákno) */
    uint32_t     rx_inflight;     /* spojení s rozpracovaným paketem */

    struct out_msg *timers;       /* seřazeno podle deadline_ms */

    int          listen_paused;
    long long    listen_resume_ms;

    int          ran;             /* run už byl zavolán */

    pthread_mutex_t mb_lock;
    struct out_msg *mb_head, *mb_tail;
    int             mb_open;      /* 0 = run skončil, send vrací KIVFS_EIO */
    int             stop;         /* kivfs_reactor_stop byl zavolán */
};

/* ------------------------------------------------------------------ */
/* Pomocné funkce                                                      */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static kivfs_conn_id make_id(uint32_t gen, uint32_t idx)
{
    return ((uint64_t)gen << 32) | idx;
}

static kivfs_conn_id conn_id_of(const struct kivfs_reactor *r, const struct conn *c)
{
    return make_id(c->gen, (uint32_t)(c - r->conns));
}

/* Najde AKTIVNÍ spojení podle id; staré/neplatné id -> NULL. */
static struct conn *conn_lookup(struct kivfs_reactor *r, kivfs_conn_id id)
{
    uint32_t idx = (uint32_t)id;
    uint32_t gen = (uint32_t)(id >> 32);
    if (gen == 0 || idx >= r->total)
        return NULL;
    struct conn *c = &r->conns[idx];
    return (c->active && c->gen == gen) ? c : NULL;
}

static void msg_free(struct out_msg *m)
{
    kivfs_pkt_free(&m->pkt);
    free(m);
}

static void msg_list_free(struct out_msg *m)
{
    while (m != NULL) {
        struct out_msg *next = m->next;
        msg_free(m);
        m = next;
    }
}

static void wake(struct kivfs_reactor *r)
{
    uint64_t one = 1;
    /* EAGAIN by znamenalo přetečení čítače (2^64 - 1) – eventfd je pak
     * stejně čitelný, I/O vlákno se probudí. Jiná chyba nastat nemůže. */
    ssize_t n;
    do {
        n = write(r->wake_fd, &one, sizeof one);
    } while (n < 0 && errno == EINTR);
}

/* ------------------------------------------------------------------ */
/* Spojení: registrace v epollu a zavření                              */
/* ------------------------------------------------------------------ */

/* Příchozí: čte se, jen když neběží požadavek. Odchozí: čte se vždy
 * (chodí jen odpovědi; nevyžádaná odpověď = porušení protokolu). */
static int reading_enabled(const struct conn *c)
{
    if (c->connecting || c->closing)
        return 0;
    return c->outgoing || !c->awaiting;
}

static void resp_fail(struct kivfs_reactor *r, kivfs_conn_id id, uint64_t tag, int err)
{
    r->cb.on_response(r->cb.user, id, tag, err, NULL);
}

static void conn_close(struct kivfs_reactor *r, struct conn *c,
                       enum kivfs_close_reason reason, int err);

/* Sladí registrované události s požadovaným stavem:
 *   EPOLLIN  – jen když se smí číst (neběží požadavek, nezavírá se)
 *   EPOLLOUT – jen když je co odeslat (jinak by level-triggered epoll
 *              hlásil zapisovatelný socket pořád dokola)
 * EPOLLERR a EPOLLHUP hlásí epoll vždy, i s maskou 0.
 * Vrací 1 = spojení žije, 0 = bylo zavřeno. */
static int conn_update_events(struct kivfs_reactor *r, struct conn *c)
{
    uint32_t want = 0;
    if (c->connecting)
        want = EPOLLOUT;          /* konec connect = socket zapisovatelný */
    else {
        if (reading_enabled(c))
            want |= EPOLLIN;
        if (c->tx_head != NULL)
            want |= EPOLLOUT;
    }
    if (want == c->ev_mask)
        return 1;

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = want;
    ev.data.u64 = conn_id_of(r, c);
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) != 0) {
        conn_close(r, c, KIVFS_CLOSE_IO_ERROR, errno);
        return 0;
    }
    c->ev_mask = want;
    return 1;
}

static void conn_close(struct kivfs_reactor *r, struct conn *c,
                       enum kivfs_close_reason reason, int err)
{
    kivfs_conn_id id = conn_id_of(r, c);
    uint32_t idx = (uint32_t)(c - r->conns);
    int out = c->outgoing;

    /* close() fd zároveň odebere z epollu (nemáme žádné dup kopie fd). */
    kivfs_sock_close(&c->fd);

    if (c->hdr_got > 0)
        r->rx_inflight--;
    kivfs_pkt_free(&c->rx);
    msg_list_free(c->tx_head);

    struct out_msg *pend = c->pend_head;
    int      had_cur = c->cur_active;
    uint64_t cur_tag = c->cur_tag;

    c->active     = 0;
    c->awaiting   = 0;
    c->closing    = 0;
    c->outgoing   = 0;
    c->connecting = 0;
    c->ev_mask    = 0;
    c->hdr_got    = 0;
    c->data_got   = 0;
    c->tx_head    = c->tx_tail = NULL;
    c->pend_head  = c->pend_tail = NULL;
    c->pend_count = 0;
    c->cur_active = 0;

    /* Odchozí: každý přijatý požadavek musí dostat právě jeden on_response,
     * a to DŘÍV než on_close (on_close je poslední událost spojení). */
    if (out) {
        int rerr = reason == KIVFS_CLOSE_SHUTDOWN ? KIVFS_ESHUTDOWN : KIVFS_ECONNLOST;
        if (had_cur)
            resp_fail(r, id, cur_tag, rerr);
        while (pend != NULL) {
            struct out_msg *next = pend->next;
            resp_fail(r, id, pend->tag, rerr);
            msg_free(pend);
            pend = next;
        }
        r->out_live--;
    }

    /* Callback dostane ještě staré id; pokud v něm (nebo kdykoli později)
     * někdo pošle na toto id, zpráva v mailboxu narazí na novou generaci
     * a zahodí se. */
    if (r->cb.on_close != NULL)
        r->cb.on_close(r->cb.user, id, out, reason, err);

    c->gen++;
    if (c->gen == 0)        /* přetečení: 0 je rezervovaná */
        c->gen = 1;

    if (out) {
        /* gen++ proběhl před uvolněním zámku -> kivfs_reactor_connect,
         * který slot vezme, uvidí už novou generaci */
        pthread_mutex_lock(&r->mb_lock);
        r->out_free[r->out_free_top++] = idx;
        pthread_mutex_unlock(&r->mb_lock);
    } else {
        r->in_free[r->in_free_top++] = idx;
    }
}

/* ------------------------------------------------------------------ */
/* Odesílání                                                           */
/* ------------------------------------------------------------------ */

/* Zapíše z fronty, kolik jde. Vrací 1 = spojení žije, 0 = zavřeno. */
static int conn_flush(struct kivfs_reactor *r, struct conn *c)
{
    while (c->tx_head != NULL) {
        struct out_msg *m = c->tx_head;
        size_t total = KIVFS_HEADER_WIRE_SIZE + (size_t)m->pkt.header.data_size;
        struct iovec iov[2];
        int cnt = 0;

        /* Hlavička i payload jedním voláním (writev) – malý paket tak
         * odejde v jednom TCP segmentu. */
        if (m->off < KIVFS_HEADER_WIRE_SIZE) {
            iov[cnt].iov_base = m->hdr + m->off;
            iov[cnt].iov_len  = KIVFS_HEADER_WIRE_SIZE - m->off;
            cnt++;
            if (m->pkt.header.data_size > 0) {
                iov[cnt].iov_base = m->pkt.data;
                iov[cnt].iov_len  = m->pkt.header.data_size;
                cnt++;
            }
        } else {
            iov[cnt].iov_base = m->pkt.data + (m->off - KIVFS_HEADER_WIRE_SIZE);
            iov[cnt].iov_len  = total - m->off;
            cnt++;
        }
        long n = kivfs_sock_writev_some(c->fd, iov, cnt);

        if (n == KIVFS_EAGAIN)
            break;                              /* počkat na EPOLLOUT */
        if (n < 0) {
            conn_close(r, c, KIVFS_CLOSE_IO_ERROR, errno);
            return 0;
        }

        m->off += (size_t)n;
        if (m->off < total)
            continue;

        /* Celý paket je v jádře. Odpověď uvolní spojení pro další
         * požadavek – až teď, ne už při zařazení: peer, který posílá
         * požadavky a nečte odpovědi, tak nenafoukne naši frontu. */
        if (!c->outgoing && kivfs_proto_is_response(m->pkt.header.request_code))
            c->awaiting = 0;

        c->tx_head = m->next;
        if (c->tx_head == NULL)
            c->tx_tail = NULL;
        msg_free(m);
    }

    if (c->tx_head == NULL && c->closing) {
        conn_close(r, c, KIVFS_CLOSE_LOCAL, 0);
        return 0;
    }
    return conn_update_events(r, c);
}

/* Odchozí spojení: pokud je volné (navázané, nic neběží), začne odesílat
 * další čekající požadavek. Vrací 1 = spojení žije, 0 = zavřeno. */
static int out_start_next(struct kivfs_reactor *r, struct conn *c)
{
    if (c->connecting || c->cur_active || c->pend_head == NULL)
        return 1;

    struct out_msg *m = c->pend_head;
    c->pend_head = m->next;
    if (c->pend_head == NULL)
        c->pend_tail = NULL;
    c->pend_count--;

    c->cur_active      = 1;
    c->cur_code        = m->pkt.header.request_code;
    c->cur_ts          = m->pkt.header.timestamp;
    c->cur_tag         = m->tag;
    c->cur_deadline_ms = m->deadline_ms;

    m->next = NULL;
    if (c->tx_head == NULL)
        c->tx_head = m;
    else
        c->tx_tail->next = m;
    c->tx_tail = m;
    return conn_flush(r, c);
}

/* ------------------------------------------------------------------ */
/* Příjem                                                              */
/* ------------------------------------------------------------------ */

/* Předá hotový paket nahoru a zastaví čtení ze spojení. */
static void conn_deliver(struct kivfs_reactor *r, struct conn *c)
{
    struct kivfs_packet pkt = c->rx;          /* vlastnictví přechází na callback */
    memset(&c->rx, 0, sizeof c->rx);
    c->hdr_got  = 0;
    c->data_got = 0;
    r->rx_inflight--;

    uint16_t code = pkt.header.request_code;

    if (c->outgoing) {
        /* Musí to být odpověď právě na rozpracovaný požadavek. */
        uint64_t ref;
        if (!c->cur_active || code != (uint16_t)(c->cur_code | KIVFS_RESPONSE_FLAG)
            || kivfs_pkt_response_ref(&pkt, &ref) != KIVFS_OK || ref != c->cur_ts) {
            kivfs_pkt_free(&pkt);
            conn_close(r, c, KIVFS_CLOSE_PROTOCOL, 0);
            return;
        }
        uint64_t tag = c->cur_tag;
        c->cur_active = 0;
        r->cb.on_response(r->cb.user, conn_id_of(r, c), tag, KIVFS_OK, &pkt);
        out_start_next(r, c);
        return;
    }

    if (kivfs_proto_is_response(code)) {      /* odpověď na příchozím spojení */
        kivfs_pkt_free(&pkt);
        conn_close(r, c, KIVFS_CLOSE_PROTOCOL, 0);
        return;
    }
    c->awaiting = 1;

    if (!conn_update_events(r, c)) {          /* EPOLLIN pryč */
        kivfs_pkt_free(&pkt);
        return;
    }
    r->cb.on_packet(r->cb.user, conn_id_of(r, c), &pkt);
}

/* Čte, dokud nedorazí celý paket nebo dokud jsou v socketu data.
 * Po jednom paketu se čtení zastaví (awaiting), takže jedno spojení
 * nezablokuje smyčku na víc než jeden paket. */
static void conn_on_readable(struct kivfs_reactor *r, struct conn *c)
{
    while (c->active && reading_enabled(c)) {
        long n;

        if (c->hdr_got < KIVFS_HEADER_WIRE_SIZE) {
            /* ---- hlavička ---- */
            n = kivfs_sock_read_some(c->fd, c->hdr + c->hdr_got,
                                     KIVFS_HEADER_WIRE_SIZE - c->hdr_got);
            if (n == KIVFS_EAGAIN)
                return;
            if (n == KIVFS_ECLOSED) {
                conn_close(r, c, c->hdr_got == 0 ? KIVFS_CLOSE_PEER
                                                 : KIVFS_CLOSE_IO_ERROR, 0);
                return;
            }
            if (n < 0) {
                conn_close(r, c, KIVFS_CLOSE_IO_ERROR, errno);
                return;
            }

            if (c->hdr_got == 0) {            /* první bajt nového paketu */
                c->rx_start_ms = now_ms();
                r->rx_inflight++;
            }
            c->hdr_got += (uint32_t)n;
            if (c->hdr_got < KIVFS_HEADER_WIRE_SIZE)
                continue;

            memset(&c->rx, 0, sizeof c->rx);
            if (kivfs_proto_header_decode(c->hdr, &c->rx.header) != KIVFS_OK) {
                /* špatný magic nebo data_size > KIVFS_MAX_PAYLOAD:
                 * hranice paketů je ztracená, nejde pokračovat */
                conn_close(r, c, KIVFS_CLOSE_PROTOCOL, 0);
                return;
            }
            c->data_got = 0;
            if (c->rx.header.data_size == 0) {
                conn_deliver(r, c);
                return;
            }
            c->rx.data = malloc(c->rx.header.data_size);
            if (c->rx.data == NULL) {
                conn_close(r, c, KIVFS_CLOSE_IO_ERROR, ENOMEM);
                return;
            }
            c->rx.capacity = c->rx.header.data_size;
        } else {
            /* ---- payload ---- */
            n = kivfs_sock_read_some(c->fd, c->rx.data + c->data_got,
                                     c->rx.header.data_size - c->data_got);
            if (n == KIVFS_EAGAIN)
                return;
            if (n == KIVFS_ECLOSED) {         /* konec uprostřed paketu */
                conn_close(r, c, KIVFS_CLOSE_IO_ERROR, 0);
                return;
            }
            if (n < 0) {
                conn_close(r, c, KIVFS_CLOSE_IO_ERROR, errno);
                return;
            }
            c->data_got += (uint32_t)n;
            if (c->data_got == c->rx.header.data_size) {
                conn_deliver(r, c);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Události jednoho spojení                                            */
/* ------------------------------------------------------------------ */

/* Odchozí spojení: neblokující connect skončil (úspěchem i neúspěchem). */
static void conn_on_connected(struct kivfs_reactor *r, struct conn *c, uint32_t events)
{
    int rc  = kivfs_sock_connect_finish(c->fd);
    int err = errno;
    if (rc == KIVFS_OK && (events & (EPOLLERR | EPOLLHUP))) {
        rc  = KIVFS_EIO;
        err = ECONNREFUSED;
    }
    if (rc != KIVFS_OK) {
        conn_close(r, c, KIVFS_CLOSE_CONNECT, err);
        return;
    }
    c->connecting = 0;
    if (r->cb.on_connect != NULL)
        r->cb.on_connect(r->cb.user, conn_id_of(r, c));
    if (!conn_update_events(r, c))
        return;
    out_start_next(r, c);
}

static void conn_on_event(struct kivfs_reactor *r, struct conn *c, uint32_t events)
{
    if (c->connecting) {
        if (events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
            conn_on_connected(r, c, events);
        return;
    }

    if (events & EPOLLERR) {
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
            err = errno;
        conn_close(r, c, KIVFS_CLOSE_IO_ERROR, err);
        return;
    }

    if ((events & EPOLLOUT) && !conn_flush(r, c))
        return;

    if (reading_enabled(c)) {
        /* EPOLLHUP bez EPOLLIN: dočte se, read pak vrátí konec spojení */
        if (events & (EPOLLIN | EPOLLHUP))
            conn_on_readable(r, c);
    } else if (events & EPOLLHUP) {
        /* Nečteme (běží požadavek) a protistrana je pryč v obou směrech –
         * odpověď už nejde doručit. Bez zavření by se level-triggered
         * EPOLLHUP hlásil pořád dokola. */
        conn_close(r, c, KIVFS_CLOSE_PEER, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Accept                                                              */
/* ------------------------------------------------------------------ */

static void listen_pause(struct kivfs_reactor *r)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = 0;
    ev.data.u64 = TOKEN_LISTEN;
    epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, r->listen_fd, &ev);
    r->listen_paused    = 1;
    r->listen_resume_ms = now_ms() + ACCEPT_BACKOFF_MS;
}

static void listen_resume(struct kivfs_reactor *r)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = EPOLLIN;
    ev.data.u64 = TOKEN_LISTEN;
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, r->listen_fd, &ev) == 0)
        r->listen_paused = 0;
    else
        r->listen_resume_ms = now_ms() + ACCEPT_BACKOFF_MS;
}

static void on_accept_ready(struct kivfs_reactor *r)
{
    for (;;) {
        char peer[KIVFS_SOCK_ADDR_STRLEN];
        int fd = kivfs_sock_accept(r->listen_fd, peer, sizeof peer);

        if (fd == KIVFS_EAGAIN)
            return;
        if (fd < 0) {
            LOG("accept: %s (%s), pauza %d ms\n",
                kivfs_strerror(fd), strerror(errno), ACCEPT_BACKOFF_MS);
            listen_pause(r);
            return;
        }

        if (r->in_free_top == 0) {
            LOG("odmítnuto spojení z %s: dosažen max_conns (%u)\n",
                peer, r->cfg.max_conns);
            kivfs_sock_close(&fd);
            continue;
        }

        uint32_t idx = r->in_free[--r->in_free_top];
        struct conn *c = &r->conns[idx];
        c->fd       = fd;
        c->active   = 1;
        c->awaiting = 0;
        c->closing  = 0;
        c->outgoing = 0;
        c->connecting = 0;
        c->hdr_got  = 0;
        c->data_got = 0;
        c->tx_head  = c->tx_tail = NULL;
        memset(&c->rx, 0, sizeof c->rx);

        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events   = EPOLLIN;
        ev.data.u64 = conn_id_of(r, c);
        if (epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            LOG("epoll_ctl ADD (%s): %s\n", peer, strerror(errno));
            kivfs_sock_close(&c->fd);
            c->active = 0;
            r->in_free[r->in_free_top++] = idx;
            continue;
        }
        c->ev_mask = EPOLLIN;

        if (r->cb.on_accept != NULL)
            r->cb.on_accept(r->cb.user, conn_id_of(r, c), peer);
    }
}

/* ------------------------------------------------------------------ */
/* Mailbox                                                             */
/* ------------------------------------------------------------------ */

/* Zařadí časovač do seznamu seřazeného podle deadline (stejné deadline
 * v pořadí zařazení). */
static void timer_insert(struct kivfs_reactor *r, struct out_msg *m)
{
    struct out_msg **pp = &r->timers;
    while (*pp != NULL && (*pp)->deadline_ms <= m->deadline_ms)
        pp = &(*pp)->next;
    m->next = *pp;
    *pp = m;
}

/* MSG_CONNECT: slot už je rezervovaný (kivfs_reactor_connect). */
static void out_connect(struct kivfs_reactor *r, struct out_msg *m)
{
    uint32_t idx = (uint32_t)m->id;
    struct conn *c = &r->conns[idx];

    c->fd         = -1;
    c->active     = 1;
    c->outgoing   = 1;
    c->connecting = 1;
    c->awaiting   = 0;
    c->closing    = 0;
    c->ev_mask    = 0;
    c->hdr_got    = 0;
    c->data_got   = 0;
    c->tx_head    = c->tx_tail = NULL;
    c->pend_head  = c->pend_tail = NULL;
    c->pend_count = 0;
    c->cur_active = 0;
    memset(&c->rx, 0, sizeof c->rx);
    r->out_live++;

    int fd = kivfs_sock_connect_start((const struct sockaddr *)&m->addr, m->addrlen);
    msg_free(m);
    if (fd < 0) {
        conn_close(r, c, KIVFS_CLOSE_CONNECT, errno);
        return;
    }
    c->fd = fd;

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = EPOLLOUT;
    ev.data.u64 = conn_id_of(r, c);
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        conn_close(r, c, KIVFS_CLOSE_CONNECT, errno);
        return;
    }
    c->ev_mask = EPOLLOUT;
    c->connect_deadline_ms = now_ms() + r->cfg.connect_timeout_ms;
}

/* MSG_REQUEST: každá cesta končí buď zařazením, nebo on_response. */
static void out_request(struct kivfs_reactor *r, struct conn *c, struct out_msg *m)
{
    int err = KIVFS_OK;
    if (c == NULL || c->closing)
        err = KIVFS_ECONNLOST;
    else if (!c->outgoing)
        err = KIVFS_EINVAL;
    else if (c->pend_count + c->cur_active >= r->cfg.max_pending)
        err = KIVFS_EAGAIN;

    if (err != KIVFS_OK) {
        resp_fail(r, m->id, m->tag, err);
        msg_free(m);
        return;
    }

    if (c->pend_tail == NULL)
        c->pend_head = m;
    else
        c->pend_tail->next = m;
    c->pend_tail = m;
    c->pend_count++;
    out_start_next(r, c);
}

/* Zpracuje zprávy od workerů. Vrací 1, pokud byl zavolán kivfs_reactor_stop.
 * stop je obyčejný int pod mb_lock (ne stdatomic): stop vždy budí přes
 * eventfd, takže stačí ho číst tady a smyčka kvůli němu nezamyká. */
static int process_mailbox(struct kivfs_reactor *r)
{
    /* Pořadí je důležité: NEJDŘÍV vyčíst eventfd, POTOM vzít seznam.
     * Opačně by se mohl ztratit budíček zprávy, která přišla mezi tím. */
    uint64_t cnt;
    while (read(r->wake_fd, &cnt, sizeof cnt) < 0 && errno == EINTR) { }

    pthread_mutex_lock(&r->mb_lock);
    struct out_msg *m = r->mb_head;
    r->mb_head = r->mb_tail = NULL;
    int stop = r->stop;
    pthread_mutex_unlock(&r->mb_lock);

    while (m != NULL) {
        struct out_msg *next = m->next;
        m->next = NULL;

        if (m->kind == MSG_TIMER) {
            timer_insert(r, m);
            m = next;
            continue;
        }
        if (m->kind == MSG_CONNECT) {
            out_connect(r, m);
            m = next;
            continue;
        }

        struct conn *c = conn_lookup(r, m->id);

        if (m->kind == MSG_REQUEST) {
            out_request(r, c, m);
        } else if (c == NULL || c->closing) {
            /* spojení zaniklo (nebo se zavírá) – tiše zahodit */
            msg_free(m);
        } else if (m->kind == MSG_SEND && c->outgoing) {
            /* odpověď na odchozí spojení nepatří – zahodit */
            LOG("send na odchozí spojení %llx zahozen\n", (unsigned long long)m->id);
            msg_free(m);
        } else if (m->kind == MSG_CLOSE && c->outgoing) {
            msg_free(m);
            conn_close(r, c, KIVFS_CLOSE_LOCAL, 0);   /* odchozí: hned */
        } else if (m->kind == MSG_SEND) {
            int was_empty = (c->tx_head == NULL);
            if (was_empty)
                c->tx_head = m;
            else
                c->tx_tail->next = m;
            c->tx_tail = m;
            /* Zkusit zapsat hned – většinou se odpověď vejde do bufferu
             * jádra a ušetří se kolečko přes EPOLLOUT. */
            if (was_empty)
                conn_flush(r, c);
        } else { /* MSG_CLOSE */
            msg_free(m);
            c->closing = 1;
            if (c->tx_head == NULL)
                conn_close(r, c, KIVFS_CLOSE_LOCAL, 0);
            else
                conn_update_events(r, c);     /* přestat číst */
        }
        m = next;
    }
    return stop;
}

/* Vloží zprávu do mailboxu. Vrací KIVFS_OK nebo KIVFS_EIO (run skončil);
 * při chybě zprávu uvolní. */
static int mailbox_post(struct kivfs_reactor *r, struct out_msg *m)
{
    int need_wake;

    m->next = NULL;
    pthread_mutex_lock(&r->mb_lock);
    if (!r->mb_open) {
        pthread_mutex_unlock(&r->mb_lock);
        msg_free(m);
        return KIVFS_EIO;
    }
    /* Budit jen při přechodu prázdný -> neprázdný: jinak už budíček
     * čeká a I/O vlákno si vezme celý seznam najednou. */
    need_wake = (r->mb_head == NULL);
    if (r->mb_tail != NULL)
        r->mb_tail->next = m;
    else
        r->mb_head = m;
    r->mb_tail = m;
    pthread_mutex_unlock(&r->mb_lock);

    if (need_wake)
        wake(r);
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Časovače                                                            */
/* ------------------------------------------------------------------ */

/* Zavře spojení, kterým vypršel rx_timeout_ms, a vrátí timeout pro další
 * epoll_wait v ms (-1 = žádný časovač neběží). */
static int run_timers(struct kivfs_reactor *r)
{
    long long now = now_ms();
    long long next = -1;

    if (r->listen_paused) {
        if (now >= r->listen_resume_ms)
            listen_resume(r);
        if (r->listen_paused)
            next = r->listen_resume_ms;
    }

#define NEXT(d) do { if (next < 0 || (d) < next) next = (d); } while (0)

    /* časovače kivfs_reactor_timer */
    while (r->timers != NULL && r->timers->deadline_ms <= now) {
        struct out_msg *t = r->timers;
        r->timers = t->next;
        r->cb.on_timer(r->cb.user, t->tag);
        msg_free(t);
    }
    if (r->timers != NULL)
        NEXT(r->timers->deadline_ms);

    /* odchozí spojení: connect timeout, timeouty požadavků */
    for (uint32_t i = r->cfg.max_conns; i < r->total && r->out_live > 0; i++) {
        struct conn *c = &r->conns[i];
        if (!c->active || !c->outgoing)
            continue;
        kivfs_conn_id id = conn_id_of(r, c);

        if (c->connecting) {
            if (now >= c->connect_deadline_ms)
                conn_close(r, c, KIVFS_CLOSE_CONNECT, ETIMEDOUT);
            else
                NEXT(c->connect_deadline_ms);
            continue;
        }

        /* Čekající požadavek s prošlým časem se jen vyřadí. */
        struct out_msg **pp = &c->pend_head;
        struct out_msg *prev = NULL;
        while (*pp != NULL) {
            struct out_msg *m = *pp;
            if (now >= m->deadline_ms) {
                *pp = m->next;
                if (c->pend_tail == m)
                    c->pend_tail = prev;
                c->pend_count--;
                resp_fail(r, id, m->tag, KIVFS_ETIMEOUT);
                msg_free(m);
            } else {
                NEXT(m->deadline_ms);
                prev = m;
                pp = &m->next;
            }
        }

        /* Rozpracovaný požadavek: protistrana nic dalšího nepřečte, dokud
         * neodpoví -> spojení je nepoužitelné, zavřít. */
        if (c->cur_active) {
            if (now >= c->cur_deadline_ms) {
                uint64_t tag = c->cur_tag;
                c->cur_active = 0;
                resp_fail(r, id, tag, KIVFS_ETIMEOUT);
                conn_close(r, c, KIVFS_CLOSE_RESP_TIMEOUT, ETIMEDOUT);
            } else {
                NEXT(c->cur_deadline_ms);
            }
        }
    }
#undef NEXT

    if (r->cfg.rx_timeout_ms > 0 && r->rx_inflight > 0) {
        /* Lineární průchod: spojení jsou stovky, ne miliony, a běží to
         * jen když nějaký paket opravdu přichází. */
        for (uint32_t i = 0; i < r->total && r->rx_inflight > 0; i++) {
            struct conn *c = &r->conns[i];
            if (!c->active || c->hdr_got == 0)
                continue;
            long long deadline = c->rx_start_ms + r->cfg.rx_timeout_ms;
            if (now >= deadline)
                conn_close(r, c, KIVFS_CLOSE_TIMEOUT, ETIMEDOUT);
            else if (next < 0 || deadline < next)
                next = deadline;
        }
    }

    if (next < 0)
        return -1;
    long long d = next - now;
    if (d < 0)
        d = 0;
    if (d > 60000)
        d = 60000;
    return (int)d;
}

/* ------------------------------------------------------------------ */
/* Veřejné API                                                         */
/* ------------------------------------------------------------------ */

static int epoll_add_token(int epfd, int fd, uint64_t token)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = EPOLLIN;
    ev.data.u64 = token;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0 ? KIVFS_OK : KIVFS_EIO;
}

int kivfs_reactor_create(struct kivfs_reactor **out,
                         const struct kivfs_reactor_config *cfg,
                         const struct kivfs_reactor_callbacks *cb)
{
    if (out == NULL)
        return KIVFS_EINVAL;
    *out = NULL;
    if (cfg == NULL || cb == NULL || cb->on_packet == NULL
        || cfg->max_conns == 0 || cfg->max_conns > 0x7FFFFFFFu
        || cfg->max_out_conns > 0x7FFFFFFFu - cfg->max_conns
        || cfg->rx_timeout_ms < 0 || cfg->connect_timeout_ms < 0)
        return KIVFS_EINVAL;

    struct kivfs_reactor *r = calloc(1, sizeof *r);
    if (r == NULL)
        return KIVFS_ENOMEM;

    r->cfg = *cfg;
    r->cfg.bind_addr = NULL;     /* používá se jen tady; volající ho nemusí držet */
    if (r->cfg.max_out_conns == 0)
        r->cfg.max_out_conns = DEFAULT_MAX_OUT_CONNS;
    if (r->cfg.max_pending == 0)
        r->cfg.max_pending = DEFAULT_MAX_PENDING;
    if (r->cfg.connect_timeout_ms == 0)
        r->cfg.connect_timeout_ms = DEFAULT_CONNECT_TIMEOUT_MS;
    r->total = r->cfg.max_conns + r->cfg.max_out_conns;
    r->cb  = *cb;
    r->listen_fd = r->epoll_fd = r->wake_fd = -1;
    r->mb_open = 1;

    int rc;
    if (pthread_mutex_init(&r->mb_lock, NULL) != 0) {
        free(r);
        return KIVFS_ENOMEM;
    }

    r->conns    = calloc(r->total, sizeof *r->conns);
    r->in_free  = calloc(r->cfg.max_conns, sizeof *r->in_free);
    r->out_free = calloc(r->cfg.max_out_conns, sizeof *r->out_free);
    if (r->conns == NULL || r->in_free == NULL || r->out_free == NULL) {
        rc = KIVFS_ENOMEM;
        goto fail;
    }
    for (uint32_t i = 0; i < r->total; i++) {
        r->conns[i].fd  = -1;
        r->conns[i].gen = 1;
    }
    /* obráceně, aby se jako první vydal nejnižší slot (hezčí v logu) */
    for (uint32_t i = 0; i < r->cfg.max_conns; i++)
        r->in_free[i] = r->cfg.max_conns - 1 - i;
    r->in_free_top = r->cfg.max_conns;
    for (uint32_t i = 0; i < r->cfg.max_out_conns; i++)
        r->out_free[i] = r->total - 1 - i;
    r->out_free_top = r->cfg.max_out_conns;

    rc = kivfs_sock_listen(cfg->bind_addr, cfg->port, cfg->backlog);
    if (rc < 0)
        goto fail;
    r->listen_fd = rc;

    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    r->wake_fd  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (r->epoll_fd < 0 || r->wake_fd < 0
        || epoll_add_token(r->epoll_fd, r->listen_fd, TOKEN_LISTEN) != KIVFS_OK
        || epoll_add_token(r->epoll_fd, r->wake_fd,   TOKEN_WAKE)   != KIVFS_OK) {
        rc = KIVFS_EIO;
        goto fail;
    }

    *out = r;
    return KIVFS_OK;

fail: {
        int saved = errno;
        kivfs_reactor_destroy(r);
        errno = saved;
        return rc;
    }
}

int kivfs_reactor_run(struct kivfs_reactor *r)
{
    if (r == NULL || r->ran)
        return KIVFS_EINVAL;
    r->ran = 1;

    struct epoll_event evs[MAX_EVENTS];
    int rc = KIVFS_OK;
    int stopping = 0;

    while (!stopping) {
        int timeout = run_timers(r);
        int n = epoll_wait(r->epoll_fd, evs, MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG("epoll_wait: %s\n", strerror(errno));
            rc = KIVFS_EIO;
            break;
        }

        for (int i = 0; i < n; i++) {
            uint64_t tok = evs[i].data.u64;
            if (tok == TOKEN_LISTEN) {
                on_accept_ready(r);
            } else if (tok == TOKEN_WAKE) {
                stopping = process_mailbox(r);
            } else {
                struct conn *c = conn_lookup(r, tok);
                if (c != NULL)          /* NULL = událost zavřeného spojení */
                    conn_on_event(r, c, evs[i].events);
            }
        }
    }

    /* ---- ukončení ---- */
    /* 1) zavřít mailbox: další send/request/connect/timer vrátí KIVFS_EIO */
    pthread_mutex_lock(&r->mb_lock);
    r->mb_open = 0;
    struct out_msg *pending = r->mb_head;
    r->mb_head = r->mb_tail = NULL;
    pthread_mutex_unlock(&r->mb_lock);

    /* 2) nezpracované zprávy: i požadavky, které reactor ještě neviděl,
     *    dostanou svůj on_response (ESHUTDOWN) a rezervovaná odchozí
     *    spojení svůj on_close – nejdřív požadavky, pak spojení. */
    for (struct out_msg *m = pending; m != NULL; m = m->next)
        if (m->kind == MSG_REQUEST)
            resp_fail(r, m->id, m->tag, KIVFS_ESHUTDOWN);
    for (struct out_msg *m = pending; m != NULL; m = m->next) {
        if (m->kind != MSG_CONNECT)
            continue;
        if (r->cb.on_close != NULL)
            r->cb.on_close(r->cb.user, m->id, 1, KIVFS_CLOSE_SHUTDOWN, 0);
        struct conn *c = &r->conns[(uint32_t)m->id];
        c->gen++;
        if (c->gen == 0)
            c->gen = 1;
    }
    msg_list_free(pending);

    /* 3) zavřít všechna spojení; neodeslané odpovědi se zahodí, odchozí
     *    požadavky dostanou ESHUTDOWN */
    for (uint32_t i = 0; i < r->total; i++)
        if (r->conns[i].active)
            conn_close(r, &r->conns[i], KIVFS_CLOSE_SHUTDOWN, 0);

    /* 4) nevypršené časovače zahodit bez callbacku */
    msg_list_free(r->timers);
    r->timers = NULL;

    /* 5) přestat přijímat – noví klienti dostanou rovnou "connection refused"
     *    místo visení v backlogu až do destroy */
    kivfs_sock_close(&r->listen_fd);
    return rc;
}

void kivfs_reactor_stop(struct kivfs_reactor *r)
{
    if (r == NULL)
        return;
    pthread_mutex_lock(&r->mb_lock);
    r->stop = 1;
    pthread_mutex_unlock(&r->mb_lock);
    wake(r);
}

void kivfs_reactor_destroy(struct kivfs_reactor *r)
{
    if (r == NULL)
        return;

    if (r->conns != NULL) {
        for (uint32_t i = 0; i < r->total; i++) {
            struct conn *c = &r->conns[i];
            if (c->active) {           /* jen když run nikdy neběžel do konce */
                kivfs_sock_close(&c->fd);
                kivfs_pkt_free(&c->rx);
                msg_list_free(c->tx_head);
                msg_list_free(c->pend_head);
            }
        }
    }
    msg_list_free(r->mb_head);
    msg_list_free(r->timers);

    kivfs_sock_close(&r->listen_fd);
    kivfs_sock_close(&r->epoll_fd);
    kivfs_sock_close(&r->wake_fd);
    pthread_mutex_destroy(&r->mb_lock);
    free(r->conns);
    free(r->in_free);
    free(r->out_free);
    free(r);
}

int kivfs_reactor_send(struct kivfs_reactor *r, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    if (pkt == NULL)
        return KIVFS_EINVAL;
    if (r == NULL) {
        kivfs_pkt_free(pkt);
        return KIVFS_EINVAL;
    }

    struct out_msg *m = malloc(sizeof *m);
    if (m == NULL) {
        kivfs_pkt_free(pkt);
        return KIVFS_ENOMEM;
    }

    m->kind = MSG_SEND;
    m->id   = id;
    m->off  = 0;
    m->pkt  = *pkt;                  /* převzetí payloadu */
    pkt->data = NULL;                /* "vyprázdnit" paket volajícího */
    pkt->capacity = 0;
    pkt->header.data_size = 0;

    /* Hlavičku zakódujeme už tady (ve vlákně workera), I/O vlákno pak
     * jen kopíruje bajty do socketu. */
    kivfs_proto_header_encode(&m->pkt.header, m->hdr);

    return mailbox_post(r, m);
}

void kivfs_reactor_close_conn(struct kivfs_reactor *r, kivfs_conn_id id)
{
    if (r == NULL)
        return;
    struct out_msg *m = calloc(1, sizeof *m);
    if (m == NULL) {
        /* void API: nemáme jak hlásit. Spojení zůstane otevřené, dokud
         * ho neukončí protistrana. */
        LOG("close_conn: ENOMEM, spojení %llx zůstává otevřené\n",
            (unsigned long long)id);
        return;
    }
    m->kind = MSG_CLOSE;
    m->id   = id;
    (void)mailbox_post(r, m);       /* po skončení run už je stejně vše zavřené */
}

int kivfs_reactor_connect(struct kivfs_reactor *r,
                          const struct sockaddr *addr, socklen_t addrlen,
                          kivfs_conn_id *out)
{
    if (out != NULL)
        *out = KIVFS_CONN_ID_INVALID;
    if (r == NULL || addr == NULL || out == NULL
        || addrlen == 0 || addrlen > sizeof(struct sockaddr_storage))
        return KIVFS_EINVAL;

    struct out_msg *m = calloc(1, sizeof *m);
    if (m == NULL)
        return KIVFS_ENOMEM;
    m->kind = MSG_CONNECT;
    memcpy(&m->addr, addr, addrlen);
    m->addrlen = addrlen;

    /* Slot se rezervuje a zpráva zařadí pod JEDNÍM zámkem: id se vrací
     * hned a požadavky zařazené potom jsou v mailboxu až za CONNECT. */
    pthread_mutex_lock(&r->mb_lock);
    if (!r->mb_open || r->out_free_top == 0) {
        int rc = r->mb_open ? KIVFS_EAGAIN : KIVFS_EIO;
        pthread_mutex_unlock(&r->mb_lock);
        free(m);
        return rc;
    }
    uint32_t idx = r->out_free[--r->out_free_top];
    kivfs_conn_id id = make_id(r->conns[idx].gen, idx);
    m->id = id;
    int need_wake = (r->mb_head == NULL);
    if (r->mb_tail != NULL)
        r->mb_tail->next = m;
    else
        r->mb_head = m;
    r->mb_tail = m;
    pthread_mutex_unlock(&r->mb_lock);

    if (need_wake)
        wake(r);
    *out = id;      /* NE m->id: po odemčení může m I/O vlákno už uvolnit */
    return KIVFS_OK;
}

int kivfs_reactor_request(struct kivfs_reactor *r, kivfs_conn_id id,
                          struct kivfs_packet *req, uint64_t tag, int timeout_ms)
{
    if (req == NULL)
        return KIVFS_EINVAL;
    if (r == NULL || timeout_ms <= 0 || r->cb.on_response == NULL
        || kivfs_proto_is_response(req->header.request_code)) {
        kivfs_pkt_free(req);
        return KIVFS_EINVAL;
    }

    struct out_msg *m = malloc(sizeof *m);
    if (m == NULL) {
        kivfs_pkt_free(req);
        return KIVFS_ENOMEM;
    }
    m->kind        = MSG_REQUEST;
    m->id          = id;
    m->off         = 0;
    m->tag         = tag;
    m->deadline_ms = now_ms() + timeout_ms;   /* timeout běží od zavolání */
    m->pkt         = *req;
    req->data = NULL;
    req->capacity = 0;
    req->header.data_size = 0;
    kivfs_proto_header_encode(&m->pkt.header, m->hdr);

    return mailbox_post(r, m);
}

int kivfs_reactor_timer(struct kivfs_reactor *r, int delay_ms, uint64_t tag)
{
    if (r == NULL || delay_ms < 0 || r->cb.on_timer == NULL)
        return KIVFS_EINVAL;
    struct out_msg *m = calloc(1, sizeof *m);
    if (m == NULL)
        return KIVFS_ENOMEM;
    m->kind        = MSG_TIMER;
    m->tag         = tag;
    m->deadline_ms = now_ms() + delay_ms;
    return mailbox_post(r, m);
}
