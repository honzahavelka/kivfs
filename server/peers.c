/*
 * KIVFS – tabulka peerů (implementace peers.h)
 * ============================================
 *
 * Zámky a co chrání:
 *
 *   link->lock       kanál (ch), next_ts, dead, retry_after_ms.
 *                    Drží ho vlákno od acquire do release – i přes connect
 *                    a čekání na odpověď. Víc linků najednou jen vzestupně
 *                    podle node_id (kivfs_peers_acquire_all).
 *   link->hint_lock  alive_hint + stavové údaje pro snapshot. Drží se jen
 *                    na přečtení/zápis pár intů, nikdy přes síť.
 *   p->in_lock       příchozí tabulka (in[], node_in[]). Krátce.
 *
 * Pořadí zamykání (kde se vnořují): in_lock -> hint_lock. link->lock se
 * s ostatními nevnořuje, kromě toho, že pod ním se krátce bere hint_lock
 * (link->lock -> hint_lock). in_lock pod link->lock nikdy, takže cyklus
 * nevznikne.
 */
#define _POSIX_C_SOURCE 200809L     /* clock_gettime */

#include "peers.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "channel.h"
#include "errors.h"
#include "log.h"
#include "protocol.h"
#include "socket.h"

#define TAG "[peers]"

/* ------------------------------------------------------------------ */
/* Datové struktury                                                    */
/* ------------------------------------------------------------------ */

struct kivfs_peer_link {
    /* neměnné po create */
    uint32_t                      node_id;
    const struct kivfs_node_addr *addr;

    /* pod lock */
    pthread_mutex_t  lock;
    struct kivfs_chan ch;
    uint64_t         next_ts;          /* čítač pro header.timestamp */
    int              dead;             /* chyba během půjčky -> release zavře */
    int64_t          retry_after_ms;   /* backoff: monotónní čas, 0 = žádný */

    /* pod hint_lock */
    pthread_mutex_t  hint_lock;
    int              alive_hint;       /* peer nám od posledního neúspěchu poslal HELLO */
    int              st_connected;     /* kopie "ch.fd >= 0" pro snapshot */
    int              st_last_err;      /* poslední chyba připojení/HELLO, 0 = žádná */
};

/* Položka příchozí tabulky, index = slot z conn_id. Platná, jen když
 * id sedí celé (generace + slot) – jinak je to zbytek po starém spojení. */
struct in_conn {
    kivfs_conn_id        id;
    enum kivfs_conn_kind kind;
    uint32_t             node_id;
};

struct kivfs_peers {
    struct kivfs_peers_config pc;
    const struct kivfs_config *cfg;
    uint64_t                   fp;            /* otisk [nodes] */

    size_t                 n_links;           /* = n_nodes - 1 */
    struct kivfs_peer_link links[KIVFS_CFG_MAX_NODES];   /* vzestupně podle node_id */

    pthread_mutex_t in_lock;
    struct in_conn *in;                       /* [cfg->max_conns] */
    kivfs_conn_id   node_in[KIVFS_CFG_MAX_NODES];  /* příchozí spojení od links[i] */
};

/* ------------------------------------------------------------------ */
/* Pomocné                                                             */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Index linku pro node_id, nebo -1 (neznámé id nebo já sám). */
static int link_index(const struct kivfs_peers *p, uint32_t node_id)
{
    for (size_t i = 0; i < p->n_links; i++)
        if (p->links[i].node_id == node_id)
            return (int)i;
    return -1;
}

static void set_status(struct kivfs_peer_link *l, int connected, int last_err)
{
    pthread_mutex_lock(&l->hint_lock);
    l->st_connected = connected;
    l->st_last_err = last_err;
    pthread_mutex_unlock(&l->hint_lock);
}

/* Přečte a vynuluje nápovědu živosti. */
static int take_hint(struct kivfs_peer_link *l)
{
    pthread_mutex_lock(&l->hint_lock);
    int h = l->alive_hint;
    l->alive_hint = 0;
    pthread_mutex_unlock(&l->hint_lock);
    return h;
}

/* ------------------------------------------------------------------ */
/* Životní cyklus                                                      */
/* ------------------------------------------------------------------ */

int kivfs_peers_create(struct kivfs_peers **out, const struct kivfs_peers_config *pc)
{
    *out = NULL;
    if (pc == NULL || pc->cfg == NULL || pc->cfg->max_conns == 0
        || pc->connect_timeout_ms <= 0 || pc->io_timeout_ms <= 0 || pc->backoff_ms < 0)
        return KIVFS_EINVAL;

    struct kivfs_peers *p = calloc(1, sizeof *p);
    if (p == NULL)
        return KIVFS_ENOMEM;
    p->in = calloc(pc->cfg->max_conns, sizeof *p->in);
    if (p->in == NULL) {
        free(p);
        return KIVFS_ENOMEM;
    }

    p->pc = *pc;
    p->cfg = pc->cfg;
    p->fp = kivfs_config_fingerprint(pc->cfg);
    pthread_mutex_init(&p->in_lock, NULL);

    /* cfg->nodes[] je seřazené, takže i links[] budou vzestupně. */
    for (size_t i = 0; i < p->cfg->n_nodes; i++) {
        const struct kivfs_node_addr *a = &p->cfg->nodes[i];
        if (a->node_id == p->cfg->self_id)
            continue;
        struct kivfs_peer_link *l = &p->links[p->n_links++];
        l->node_id = a->node_id;
        l->addr = a;
        pthread_mutex_init(&l->lock, NULL);
        pthread_mutex_init(&l->hint_lock, NULL);
        kivfs_chan_init(&l->ch);
    }
    *out = p;
    return KIVFS_OK;
}

void kivfs_peers_destroy(struct kivfs_peers *p)
{
    if (p == NULL)
        return;
    for (size_t i = 0; i < p->n_links; i++) {
        kivfs_chan_close(&p->links[i].ch);
        pthread_mutex_destroy(&p->links[i].lock);
        pthread_mutex_destroy(&p->links[i].hint_lock);
    }
    pthread_mutex_destroy(&p->in_lock);
    free(p->in);
    free(p);
}

/* ------------------------------------------------------------------ */
/* A) ODCHOZÍ                                                          */
/* ------------------------------------------------------------------ */

/* Připojí kanál a provede HELLO. Volá se pod l->lock se zavřeným kanálem.
 * Vrací KIVFS_OK, nebo kód pro volajícího (EPEERDOWN / EPEERREJECT /
 * ENOMEM); *detail = konkrétní příčina pro log a snapshot, *sys_err =
 * errno u KIVFS_EIO (jinak 0). */
static int connect_and_hello(struct kivfs_peers *p, struct kivfs_peer_link *l, int *detail,
                             int *sys_err)
{
    const struct kivfs_node_addr *a = l->addr;
    *sys_err = 0;
    int rc = kivfs_chan_open(&l->ch, a->host, a->port, p->pc.connect_timeout_ms);
    if (rc != KIVFS_OK) {
        if (rc == KIVFS_EIO)
            *sys_err = errno;
        *detail = rc;
        return KIVFS_EPEERDOWN;
    }

    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, KIVFS_REQ_HELLO, ++l->next_ts);
    if ((rc = kivfs_pkt_add_u32(&req, p->cfg->self_id)) != KIVFS_OK
        || (rc = kivfs_pkt_add_u64(&req, p->fp)) != KIVFS_OK) {
        kivfs_pkt_free(&req);
        kivfs_chan_close(&l->ch);
        *detail = rc;
        return rc;
    }
    rc = kivfs_chan_call(&l->ch, &req, &resp);
    kivfs_pkt_free(&req);
    if (rc != KIVFS_OK) {
        if (rc == KIVFS_EIO)
            *sys_err = errno;
        kivfs_chan_close(&l->ch);
        *detail = rc;
        return KIVFS_EPEERDOWN;
    }

    /* Odpověď: [u64 ref][u32 node_id] a status OK. */
    struct kivfs_pkt_reader r;
    uint64_t ref;
    uint32_t their_id = 0;
    int ok = resp.header.return_value == KIVFS_STATUS_OK;
    if (ok) {
        kivfs_pkt_reader_init(&r, &resp);
        ok = kivfs_pkt_read_u64(&r, &ref) == KIVFS_OK
             && kivfs_pkt_read_u32(&r, &their_id) == KIVFS_OK
             && their_id == l->node_id;
    }
    if (!ok) {
        if (resp.header.return_value != KIVFS_STATUS_OK)
            kivfs_log(TAG, "uzel %u (%s:%u) odmítl HELLO: %s", (unsigned)l->node_id,
                      a->host, (unsigned)a->port, kivfs_status_str(resp.header.return_value));
        else
            kivfs_log(TAG, "na adrese %s:%u odpověděl uzel %u, čekali jsme %u -> zavírám",
                      a->host, (unsigned)a->port, (unsigned)their_id, (unsigned)l->node_id);
        kivfs_pkt_free(&resp);
        kivfs_chan_close(&l->ch);
        *detail = KIVFS_EPEERREJECT;
        return KIVFS_EPEERREJECT;
    }
    kivfs_pkt_free(&resp);

    /* HELLO prošlo; od teď platí delší timeout pro běžné dotazy. */
    if ((rc = kivfs_sock_set_timeout(l->ch.fd, p->pc.io_timeout_ms)) != KIVFS_OK) {
        kivfs_chan_close(&l->ch);
        *detail = rc;
        return KIVFS_EPEERDOWN;
    }
    l->ch.peer_node_id = l->node_id;
    *detail = KIVFS_OK;
    return KIVFS_OK;
}

/* Zamkne link a zajistí otevřený kanál. Při chybě link odemkne. */
static int acquire_link(struct kivfs_peers *p, struct kivfs_peer_link *l)
{
    pthread_mutex_lock(&l->lock);
    l->dead = 0;

    if (l->ch.fd >= 0) {
        if (!kivfs_chan_is_stale(&l->ch))
            return KIVFS_OK;
        kivfs_log(TAG, "kanál k uzlu %u je zastaralý (protistrana ho zavřela) -> připojuji znovu",
                  (unsigned)l->node_id);
        kivfs_chan_close(&l->ch);
        set_status(l, 0, 0);
    }

    /* Backoff: po neúspěchu chvíli nezkoušet – klient dostane BUSY hned,
     * ne po connect timeoutu. Výjimka: peer nám mezitím poslal HELLO. */
    int hint = take_hint(l);
    if (!hint && l->retry_after_ms != 0 && now_ms() < l->retry_after_ms) {
        pthread_mutex_unlock(&l->lock);
        return KIVFS_EPEERDOWN;
    }

    int detail = KIVFS_OK, sys_err = 0;
    int rc = connect_and_hello(p, l, &detail, &sys_err);
    if (rc == KIVFS_OK) {
        l->retry_after_ms = 0;
        set_status(l, 1, 0);
        kivfs_log(TAG, "spojeno s uzlem %u (%s)%s", (unsigned)l->node_id, l->ch.peer_addr,
                  hint ? " [nápověda z jeho HELLO]" : "");
        return KIVFS_OK;
    }

    /* Neúspěch logovat jen při změně příčiny, ať log nezahltí opakované
     * pokusy na uzel, který dlouho neběží. */
    pthread_mutex_lock(&l->hint_lock);
    int prev = l->st_last_err;
    pthread_mutex_unlock(&l->hint_lock);
    /* Odmítnutí už zalogoval connect_and_hello s podrobnostmi. */
    if (prev != detail && detail != KIVFS_EPEERREJECT)
        kivfs_log(TAG, "uzel %u (%s:%u) nedostupný: %s%s%s", (unsigned)l->node_id,
                  l->addr->host, (unsigned)l->addr->port, kivfs_strerror(detail),
                  sys_err ? " - " : "", sys_err ? strerror(sys_err) : "");
    l->retry_after_ms = now_ms() + p->pc.backoff_ms;
    if (l->retry_after_ms == 0)
        l->retry_after_ms = 1;
    set_status(l, 0, detail);
    pthread_mutex_unlock(&l->lock);
    return rc;
}

void kivfs_peers_connect_all(struct kivfs_peers *p)
{
    for (size_t i = 0; i < p->n_links; i++) {
        struct kivfs_peer_link *l = NULL;
        if (kivfs_peers_acquire(p, p->links[i].node_id, &l) == KIVFS_OK)
            kivfs_peers_release(p, l);
        /* výsledek už zalogoval acquire_link */
    }
}

int kivfs_peers_acquire(struct kivfs_peers *p, uint32_t node_id,
                        struct kivfs_peer_link **out)
{
    *out = NULL;
    int i = link_index(p, node_id);
    if (i < 0)
        return KIVFS_EINVAL;
    int rc = acquire_link(p, &p->links[i]);
    if (rc == KIVFS_OK)
        *out = &p->links[i];
    return rc;
}

int kivfs_peers_acquire_all(struct kivfs_peers *p, struct kivfs_peer_link **out,
                            size_t *n, uint32_t *failed_node)
{
    *n = 0;
    if (failed_node != NULL)
        *failed_node = KIVFS_NODE_ID_UNKNOWN;
    /* links[] jsou vzestupně podle node_id -> zamykání ve stálém pořadí. */
    for (size_t i = 0; i < p->n_links; i++) {
        int rc = acquire_link(p, &p->links[i]);
        if (rc != KIVFS_OK) {
            if (failed_node != NULL)
                *failed_node = p->links[i].node_id;
            kivfs_peers_release_all(p, out, *n);
            *n = 0;
            return rc;
        }
        out[(*n)++] = &p->links[i];
    }
    return KIVFS_OK;
}

void kivfs_peers_release(struct kivfs_peers *p, struct kivfs_peer_link *l)
{
    if (l == NULL)
        return;
    if (l->dead) {
        /* Chyba uprostřed dotazu: kanál je nepoužitelný (channel.h). Peer
         * má zřejmě potíže -> backoff, ať další dotazy nečekají na timeout. */
        kivfs_log(TAG, "kanál k uzlu %u zavřen po chybě", (unsigned)l->node_id);
        kivfs_chan_close(&l->ch);
        l->retry_after_ms = now_ms() + p->pc.backoff_ms;
        if (l->retry_after_ms == 0)
            l->retry_after_ms = 1;
        set_status(l, 0, l->dead);
        l->dead = 0;
    }
    pthread_mutex_unlock(&l->lock);
}

void kivfs_peers_release_all(struct kivfs_peers *p, struct kivfs_peer_link **l, size_t n)
{
    /* Odemykat v opačném pořadí – není nutné pro správnost, ale je to zvyk. */
    while (n > 0)
        kivfs_peers_release(p, l[--n]);
}

uint32_t kivfs_link_node_id(const struct kivfs_peer_link *l)
{
    return l->node_id;
}

int kivfs_link_send(struct kivfs_peer_link *l, struct kivfs_packet *req)
{
    if (l->dead)
        return KIVFS_EIO;
    req->header.timestamp = ++l->next_ts;
    int rc = kivfs_chan_send(&l->ch, req);
    if (rc != KIVFS_OK)
        l->dead = rc;
    return rc;
}

int kivfs_link_recv(struct kivfs_peer_link *l, const struct kivfs_packet *req,
                    struct kivfs_packet *resp)
{
    if (l->dead) {
        kivfs_pkt_init(resp, 0, 0);
        return KIVFS_EIO;
    }
    int rc = kivfs_chan_recv_response(&l->ch, req, resp);
    if (rc != KIVFS_OK)
        l->dead = rc;
    return rc;
}

int kivfs_link_call(struct kivfs_peer_link *l, struct kivfs_packet *req,
                    struct kivfs_packet *resp)
{
    int rc = kivfs_link_send(l, req);
    if (rc != KIVFS_OK) {
        kivfs_pkt_init(resp, 0, 0);
        return rc;
    }
    return kivfs_link_recv(l, req, resp);
}

/* ------------------------------------------------------------------ */
/* B) PŘÍCHOZÍ                                                         */
/* ------------------------------------------------------------------ */

/* Položka tabulky pro id, nebo NULL (slot mimo rozsah). Pod in_lock. */
static struct in_conn *in_slot(struct kivfs_peers *p, kivfs_conn_id id)
{
    uint32_t slot = (uint32_t)(id & 0xFFFFFFFFu);
    if (id == KIVFS_CONN_ID_INVALID || slot >= p->cfg->max_conns)
        return NULL;
    return &p->in[slot];
}

enum kivfs_conn_kind kivfs_peers_in_kind(struct kivfs_peers *p,
                                         kivfs_conn_id id, uint32_t *node_id)
{
    enum kivfs_conn_kind k = KIVFS_CONN_UNKNOWN;
    uint32_t nid = KIVFS_NODE_ID_UNKNOWN;

    pthread_mutex_lock(&p->in_lock);
    struct in_conn *e = in_slot(p, id);
    if (e != NULL && e->id == id) {
        k = e->kind;
        nid = e->node_id;
    }
    pthread_mutex_unlock(&p->in_lock);

    if (node_id != NULL)
        *node_id = nid;
    return k;
}

int kivfs_peers_in_hello(struct kivfs_peers *p, kivfs_conn_id id,
                         const struct kivfs_packet *req, uint64_t resp_ts,
                         struct kivfs_packet *resp, struct kivfs_hello_result *res)
{
    memset(res, 0, sizeof *res);
    res->replaced = KIVFS_CONN_ID_INVALID;

    /* 1. payload: klient [u32 0], uzel [u32 id][u64 otisk] */
    struct kivfs_pkt_reader r;
    uint32_t nid = 0;
    uint64_t fp = 0;
    int16_t status = KIVFS_STATUS_OK;

    kivfs_pkt_reader_init(&r, req);
    if (kivfs_pkt_read_u32(&r, &nid) != KIVFS_OK
        || (nid != KIVFS_NODE_ID_UNKNOWN && kivfs_pkt_read_u64(&r, &fp) != KIVFS_OK)
        || !kivfs_pkt_reader_at_end(&r)) {
        status = KIVFS_STATUS_MALFORMED;
        snprintf(res->note, sizeof res->note, "neplatný payload");
    }

    /* 2. validace a zápis do tabulky – PŘED odesláním odpovědi (peers.h) */
    if (status == KIVFS_STATUS_OK) {
        int li = nid == KIVFS_NODE_ID_UNKNOWN ? -1 : link_index(p, nid);
        enum kivfs_conn_kind kind = nid == KIVFS_NODE_ID_UNKNOWN ? KIVFS_CONN_CLIENT
                                                                 : KIVFS_CONN_NODE;

        pthread_mutex_lock(&p->in_lock);
        struct in_conn *e = in_slot(p, id);
        if (e == NULL) {
            status = KIVFS_STATUS_INTERNAL;           /* reactor porušil záruku */
            snprintf(res->note, sizeof res->note, "conn_id mimo tabulku");
        } else if (e->id == id) {
            /* Opakované HELLO na už představeném spojení: stejná identita
             * je neškodná (klientský příkaz "hello"), změna identity ne. */
            if (e->kind != kind || e->node_id != nid) {
                status = KIVFS_STATUS_MALFORMED;
                snprintf(res->note, sizeof res->note, "HELLO mění identitu spojení (%u -> %u)",
                         (unsigned)e->node_id, (unsigned)nid);
            } else {
                snprintf(res->note, sizeof res->note, "opakované HELLO od %s %u",
                         kind == KIVFS_CONN_CLIENT ? "klienta" : "uzlu", (unsigned)nid);
            }
        } else if (kind == KIVFS_CONN_CLIENT) {
            *e = (struct in_conn){ id, KIVFS_CONN_CLIENT, KIVFS_NODE_ID_UNKNOWN };
            snprintf(res->note, sizeof res->note, "klient");
        } else if (li < 0) {
            status = KIVFS_STATUS_REJECTED;
            snprintf(res->note, sizeof res->note, "node_id %u %s", (unsigned)nid,
                     nid == p->cfg->self_id ? "je moje vlastní" : "není v konfiguraci");
        } else if (fp != p->fp) {
            status = KIVFS_STATUS_REJECTED;
            snprintf(res->note, sizeof res->note,
                     "uzel %u má jiný seznam uzlů (otisk %016llx, můj %016llx)", (unsigned)nid,
                     (unsigned long long)fp, (unsigned long long)p->fp);
        } else {
            kivfs_conn_id old = p->node_in[li];
            if (old != KIVFS_CONN_ID_INVALID && old != id) {
                /* Novější vyhrává: staré spojení mohlo zůstat viset po pádu
                 * bez FIN. Zavře ho volající; z tabulky ho smaže on_close. */
                res->replaced = old;
                snprintf(res->note, sizeof res->note,
                         "uzel %u, NAHRAZUJE staré spojení " KIVFS_CONN_FMT,
                         (unsigned)nid, KIVFS_CONN_ARGS(old));
            } else {
                snprintf(res->note, sizeof res->note, "uzel %u", (unsigned)nid);
            }
            *e = (struct in_conn){ id, KIVFS_CONN_NODE, nid };
            p->node_in[li] = id;

            /* Nápověda živosti pro odchozí kanál k témuž uzlu. */
            struct kivfs_peer_link *l = &p->links[li];
            pthread_mutex_lock(&l->hint_lock);
            l->alive_hint = 1;
            pthread_mutex_unlock(&l->hint_lock);
        }
        pthread_mutex_unlock(&p->in_lock);
    }

    /* 3. odpověď */
    res->close_after = status != KIVFS_STATUS_OK;
    int rc = kivfs_pkt_init_response(resp, req, resp_ts, status);
    if (rc != KIVFS_OK)
        return rc;
    if (status == KIVFS_STATUS_OK && (rc = kivfs_pkt_add_u32(resp, p->cfg->self_id)) != KIVFS_OK) {
        kivfs_pkt_free(resp);
        return rc;
    }
    return KIVFS_OK;
}

void kivfs_peers_in_closed(struct kivfs_peers *p, kivfs_conn_id id)
{
    pthread_mutex_lock(&p->in_lock);
    struct in_conn *e = in_slot(p, id);
    if (e != NULL && e->id == id) {
        if (e->kind == KIVFS_CONN_NODE) {
            int li = link_index(p, e->node_id);
            /* Jen pokud ho mezitím nenahradilo novější spojení. */
            if (li >= 0 && p->node_in[li] == id)
                p->node_in[li] = KIVFS_CONN_ID_INVALID;
        }
        memset(e, 0, sizeof *e);
    }
    pthread_mutex_unlock(&p->in_lock);
}

/* ------------------------------------------------------------------ */
/* Diagnostika                                                         */
/* ------------------------------------------------------------------ */

size_t kivfs_peers_snapshot(struct kivfs_peers *p, struct kivfs_peer_status *out,
                            size_t max)
{
    size_t n = p->n_links < max ? p->n_links : max;
    for (size_t i = 0; i < n; i++) {
        struct kivfs_peer_link *l = &p->links[i];
        out[i].node_id = l->node_id;

        pthread_mutex_lock(&l->hint_lock);
        out[i].out_connected = l->st_connected;
        out[i].out_last_err = l->st_last_err;
        pthread_mutex_unlock(&l->hint_lock);

        pthread_mutex_lock(&p->in_lock);
        out[i].in_connected = p->node_in[i] != KIVFS_CONN_ID_INVALID;
        pthread_mutex_unlock(&p->in_lock);
    }
    return n;
}
