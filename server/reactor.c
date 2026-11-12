/*
 * KIVFS – epoll reactor (implementace), viz reactor.h
 * ===================================================
 *
 * Vnitřní uspořádání:
 *
 *   conns[max_conns]   tabulka slotů spojení. Sahá na ni JEN I/O vlákno.
 *   free_stack         volné indexy slotů (zásobník -> O(1) alokace).
 *   mailbox            jednosměrný seznam zpráv od workerů (SEND / CLOSE),
 *                      chráněný mutexem; I/O vlákno ho celý "ukradne"
 *                      (prohození hlavy) a zpracuje bez zámku.
 *   wake_fd            eventfd v epollu; zápis do něj probudí epoll_wait.
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

#include "reactor.h"
#include "errors.h"
#include "socket.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Kolik událostí vybrat jedním epoll_wait. */
#define MAX_EVENTS               64

/* Jak dlouho nepřijímat nová spojení po chybě accept (typicky EMFILE –
 * došly file deskriptory). Bez pauzy by level-triggered epoll hlásil
 * čekající spojení pořád dokola a smyčka by se točila na 100 % CPU. */
#define ACCEPT_BACKOFF_MS        100

#define TOKEN_LISTEN             ((uint64_t)1)   /* generace 0, slot 1 */
#define TOKEN_WAKE               ((uint64_t)2)   /* generace 0, slot 2 */

#define LOG(...)  fprintf(stderr, "[reactor] " __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Struktury                                                           */
/* ------------------------------------------------------------------ */

enum msg_kind { MSG_SEND, MSG_CLOSE };

struct out_msg {
    struct out_msg     *next;
    enum msg_kind       kind;
    kivfs_conn_id       id;
    /* jen MSG_SEND: */
    uint8_t             hdr[KIVFS_HEADER_WIRE_SIZE];  /* zakódovaná hlavička */
    struct kivfs_packet pkt;
    size_t              off;      /* kolik bajtů (hlavička + payload) už odešlo */
};

struct conn {
    int       fd;
    uint32_t  gen;                /* generace slotu, nikdy 0 */
    uint8_t   active;
    uint8_t   awaiting;           /* doručen požadavek, čeká se na odpověď */
    uint8_t   closing;            /* close_conn: zavřít po odeslání fronty */
    uint32_t  ev_mask;            /* události aktuálně registrované v epollu */

    /* příjem rozpracovaného paketu */
    uint8_t   hdr[KIVFS_HEADER_WIRE_SIZE];
    uint32_t  hdr_got;            /* 0 = žádný paket není rozpracovaný */
    struct kivfs_packet rx;       /* platný až po dekódování hlavičky */
    uint32_t  data_got;
    long long rx_start_ms;        /* čas 1. bajtu paketu (rx_timeout_ms) */

    /* fronta k odeslání */
    struct out_msg *tx_head, *tx_tail;
};

struct kivfs_reactor {
    struct kivfs_reactor_config    cfg;
    struct kivfs_reactor_callbacks cb;

    int listen_fd;
    int epoll_fd;
    int wake_fd;

    struct conn *conns;
    uint32_t    *free_stack;
    uint32_t     free_top;        /* počet volných slotů */
    uint32_t     rx_inflight;     /* spojení s rozpracovaným paketem */

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
    if (gen == 0 || idx >= r->cfg.max_conns)
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

static int reading_enabled(const struct conn *c)
{
    return !c->awaiting && !c->closing;
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
    if (reading_enabled(c))
        want |= EPOLLIN;
    if (c->tx_head != NULL)
        want |= EPOLLOUT;
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

    /* close() fd zároveň odebere z epollu (nemáme žádné dup kopie fd). */
    kivfs_sock_close(&c->fd);

    if (c->hdr_got > 0)
        r->rx_inflight--;
    kivfs_pkt_free(&c->rx);
    msg_list_free(c->tx_head);

    c->active   = 0;
    c->awaiting = 0;
    c->closing  = 0;
    c->ev_mask  = 0;
    c->hdr_got  = 0;
    c->data_got = 0;
    c->tx_head  = c->tx_tail = NULL;

    /* Callback dostane ještě staré id; pokud v něm (nebo kdykoli později)
     * někdo pošle na toto id, zpráva v mailboxu narazí na novou generaci
     * a zahodí se. */
    if (r->cb.on_close != NULL)
        r->cb.on_close(r->cb.user, id, reason, err);

    c->gen++;
    if (c->gen == 0)        /* přetečení: 0 je rezervovaná */
        c->gen = 1;
    r->free_stack[r->free_top++] = idx;
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
        long n;

        if (m->off < KIVFS_HEADER_WIRE_SIZE)
            n = kivfs_sock_write_some(c->fd, m->hdr + m->off,
                                      KIVFS_HEADER_WIRE_SIZE - m->off);
        else
            n = kivfs_sock_write_some(c->fd,
                                      m->pkt.data + (m->off - KIVFS_HEADER_WIRE_SIZE),
                                      total - m->off);

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
        if (kivfs_proto_is_response(m->pkt.header.request_code))
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

static void conn_on_event(struct kivfs_reactor *r, struct conn *c, uint32_t events)
{
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

        if (r->free_top == 0) {
            LOG("odmítnuto spojení z %s: dosažen max_conns (%u)\n",
                peer, r->cfg.max_conns);
            kivfs_sock_close(&fd);
            continue;
        }

        uint32_t idx = r->free_stack[--r->free_top];
        struct conn *c = &r->conns[idx];
        c->fd       = fd;
        c->active   = 1;
        c->awaiting = 0;
        c->closing  = 0;
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
            r->free_stack[r->free_top++] = idx;
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
        struct conn *c = conn_lookup(r, m->id);
        m->next = NULL;

        if (c == NULL || c->closing) {
            /* spojení zaniklo (nebo se zavírá) – tiše zahodit */
            msg_free(m);
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

    if (r->cfg.rx_timeout_ms > 0 && r->rx_inflight > 0) {
        /* Lineární průchod: max_conns jsou stovky, ne miliony, a běží to
         * jen když nějaký paket opravdu přichází. */
        for (uint32_t i = 0; i < r->cfg.max_conns && r->rx_inflight > 0; i++) {
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
        || cfg->rx_timeout_ms < 0)
        return KIVFS_EINVAL;

    struct kivfs_reactor *r = calloc(1, sizeof *r);
    if (r == NULL)
        return KIVFS_ENOMEM;

    r->cfg = *cfg;
    r->cfg.bind_addr = NULL;     /* používá se jen tady; volající ho nemusí držet */
    r->cb  = *cb;
    r->listen_fd = r->epoll_fd = r->wake_fd = -1;
    r->mb_open = 1;

    int rc;
    if (pthread_mutex_init(&r->mb_lock, NULL) != 0) {
        free(r);
        return KIVFS_ENOMEM;
    }

    r->conns      = calloc(cfg->max_conns, sizeof *r->conns);
    r->free_stack = calloc(cfg->max_conns, sizeof *r->free_stack);
    if (r->conns == NULL || r->free_stack == NULL) {
        rc = KIVFS_ENOMEM;
        goto fail;
    }
    for (uint32_t i = 0; i < cfg->max_conns; i++) {
        r->conns[i].fd  = -1;
        r->conns[i].gen = 1;
        /* obráceně, aby se jako první vydal slot 0 (hezčí v logu) */
        r->free_stack[i] = cfg->max_conns - 1 - i;
    }
    r->free_top = cfg->max_conns;

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
    /* 1) zavřít mailbox: další send vrátí KIVFS_EIO */
    pthread_mutex_lock(&r->mb_lock);
    r->mb_open = 0;
    struct out_msg *pending = r->mb_head;
    r->mb_head = r->mb_tail = NULL;
    pthread_mutex_unlock(&r->mb_lock);
    msg_list_free(pending);

    /* 2) zavřít všechna spojení; neodeslané odpovědi se zahodí */
    for (uint32_t i = 0; i < r->cfg.max_conns; i++)
        if (r->conns[i].active)
            conn_close(r, &r->conns[i], KIVFS_CLOSE_SHUTDOWN, 0);

    /* 3) přestat přijímat – noví klienti dostanou rovnou "connection refused"
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
        for (uint32_t i = 0; i < r->cfg.max_conns; i++) {
            struct conn *c = &r->conns[i];
            if (c->active) {           /* jen když run nikdy neběžel do konce */
                kivfs_sock_close(&c->fd);
                kivfs_pkt_free(&c->rx);
                msg_list_free(c->tx_head);
            }
        }
    }
    msg_list_free(r->mb_head);

    kivfs_sock_close(&r->listen_fd);
    kivfs_sock_close(&r->epoll_fd);
    kivfs_sock_close(&r->wake_fd);
    pthread_mutex_destroy(&r->mb_lock);
    free(r->conns);
    free(r->free_stack);
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
