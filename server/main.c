/*
 * KIVFS – server uzlu (krok 1: peery a HELLO mezi uzly, zatím bez Sync)
 * =====================================================================
 *
 * docker compose run --rm build && docker compose restart node1 node2 node3
 *
 * Použití:  kivfs-server <konfigurační soubor>
 *
 * Veškerá konfigurace (vlastní id, port, workery, seznam uzlů) je v souboru,
 * formát viz config.h. Ukázky pro lokální cluster: conf/local-node*.conf.
 *
 * Vlákna:
 *   main      nastaví vše, připojí se k peerům, pak spí v sigwait;
 *             po SIGINT/SIGTERM uklidí
 *   [io]      kivfs_reactor_run: sockety, skládání paketů, odesílání
 *   [w0..wN]  klientští workery: požadavky od CLI klientů; smí blokovat
 *             voláním jiných uzlů (peers.h)
 *   [n0..nM]  uzloví workery: požadavky od jiných uzlů + každé HELLO;
 *             NIKDY nečekají na jiný uzel (jinak hrozí distribuovaný
 *             deadlock, viz queue.h). Je jich n_nodes - 1: od každého peeru
 *             je jedno příchozí spojení s nejvýš jedním požadavkem.
 *
 * Tok požadavku:
 *   [io] on_packet -> podle spojení do klientské / uzlové fronty
 *   -> worker handle_request -> kivfs_reactor_send -> mailbox -> [io] zapíše
 *
 * Rozdělení do front (on_packet, podle peers.h):
 *   HELLO                         -> uzlová fronta (zapíše, kdo je na spojení)
 *   spojení KLIENT                -> klientská fronta
 *   spojení UZEL(x)               -> uzlová fronta
 *   cokoli jiného před HELLO      -> zavřít spojení ("HELLO první")
 *
 * Signály: SIGINT/SIGTERM jsou zablokované ve VŠECH vláknech (maska se
 * nastaví před vytvořením vláken a ta ji zdědí). Main si je vyzvedne
 * synchronně přes sigwait, takže kivfs_reactor_stop se volá z běžného
 * kódu, ne ze signal handleru (tam by zamykání mutexu nebylo bezpečné).
 * Signál, který přijde během kivfs_peers_connect_all, se neztratí – čeká
 * zablokovaný, dokud main nedojde do sigwait.
 *
 * PROVIZORNÍ (nahradí další kroky):
 *   - Lamportovy hodiny (clock_*) -> sync.h
 *   - handle_* a switch v handle_request -> rozhraní handlerů (dispatch)
 *   - log vždy zapnutý
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "errors.h"
#include "log.h"
#include "packet.h"
#include "peers.h"
#include "protocol.h"
#include "queue.h"
#include "reactor.h"
#include "socket.h"

#define LISTEN_BACKLOG       64
#define RX_TIMEOUT_MS        10000

/* Kanály k ostatním uzlům (peers.h). */
#define PEER_CONNECT_TIMEOUT_MS  1000
#define PEER_IO_TIMEOUT_MS       5000
#define PEER_BACKOFF_MS          1500

/* ------------------------------------------------------------------ */
/* Datové struktury                                                    */
/* ------------------------------------------------------------------ */

/* Položka fronty: kdo poslal + co poslal. Fronta položku kopíruje, tím
 * na ni přechází i vlastnictví pkt.data (a při pop zase na workera).
 * node_id se zkopíruje v on_packet, aby worker nemusel sahat do tabulky
 * spojení (0 = klient nebo ještě nepředstavené spojení s HELLO). */
struct job {
    kivfs_conn_id       conn;
    uint32_t            node_id;
    struct kivfs_packet pkt;
};

/* Celý stav serveru; ukazatel na něj je `user` v callbacích reactoru. */
struct server {
    const struct kivfs_config *cfg;     /* načtená konfigurace (žije v main) */
    uint32_t              node_id;
    struct kivfs_reactor *reactor;
    struct kivfs_peers   *peers;
    struct kivfs_queue   *client_queue; /* od klientů; workery smí blokovat */
    struct kivfs_queue   *node_queue;   /* od uzlů + HELLO; workery nikdy nečekají na uzel */

    pthread_mutex_t       clock_lock;   /* PROVIZORNÍ Lamportovy hodiny, */
    uint64_t              clock;        /* přesunou se do sync.h          */
};

struct worker_arg {
    struct server      *srv;
    struct kivfs_queue *queue;
    int                 is_node;        /* 1 = uzlový worker (nesmí volat peery) */
    unsigned            id;             /* jen pro log [wN] / [nN] */
};

/* Co má worker udělat po odeslání odpovědi (HELLO). */
struct post_action {
    int           close_conn;           /* zavřít spojení požadavku */
    kivfs_conn_id close_other;          /* zavřít jiné spojení (nahrazené) */
};

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

static const char *req_name(uint16_t code)
{
    switch (kivfs_proto_base_code(code)) {
    case KIVFS_REQ_HELLO: return "HELLO";
    case KIVFS_REQ_PING:  return "PING";
    case KIVFS_REQ_NODES: return "NODES";
    }
    return "?";
}

static const char *close_reason_str(enum kivfs_close_reason r)
{
    switch (r) {
    case KIVFS_CLOSE_PEER:     return "peer zavřel";
    case KIVFS_CLOSE_IO_ERROR: return "chyba I/O";
    case KIVFS_CLOSE_PROTOCOL: return "porušení protokolu";
    case KIVFS_CLOSE_TIMEOUT:  return "timeout příjmu";
    case KIVFS_CLOSE_LOCAL:    return "zavřeno serverem";
    case KIVFS_CLOSE_SHUTDOWN: return "server se vypíná";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Lamportovy hodiny (PROVIZORNÍ – nahradí sync.h)                     */
/* Pod mutexem, protože je volá I/O vlákno i všichni workery.          */
/* ------------------------------------------------------------------ */

/* Příjem zprávy: hodiny nesmí být pozadu za odesílatelem. */
static void clock_observe(struct server *s, uint64_t ts)
{
    pthread_mutex_lock(&s->clock_lock);
    if (ts > s->clock)
        s->clock = ts;
    pthread_mutex_unlock(&s->clock_lock);
}

/* Odeslání zprávy: zvýšit PŘED odesláním a použít novou hodnotu. */
static uint64_t clock_next(struct server *s)
{
    pthread_mutex_lock(&s->clock_lock);
    uint64_t v = ++s->clock;
    pthread_mutex_unlock(&s->clock_lock);
    return v;
}

/* ------------------------------------------------------------------ */
/* Handlery – běží ve workeru                                          */
/* Každý vrací KIVFS_OK (resp je vyplněná odpověď, i chybová) nebo     */
/* KIVFS_ENOMEM (resp je prázdný, odpovědět nelze).                    */
/* note: krátký popis pro log.                                         */
/* ------------------------------------------------------------------ */

/* HELLO – celé ho vyřídí peers.h (validace, tabulka spojení, nápověda
 * živosti). Tady se jen převezme, co se má po odeslání zavřít. */
static int handle_hello(struct server *s, const struct job *j, struct kivfs_packet *resp,
                        char *note, size_t note_size, struct post_action *post)
{
    struct kivfs_hello_result res;
    int rc = kivfs_peers_in_hello(s->peers, j->conn, &j->pkt, clock_next(s), resp, &res);
    snprintf(note, note_size, "%s", res.note);
    post->close_conn = res.close_after;
    post->close_other = res.replaced;
    return rc;
}

/* PING    požadavek: 0..N záznamů    odpověď: [u64 ref] + kopie záznamů */
static int handle_ping(struct server *s, const struct kivfs_packet *req,
                       struct kivfs_packet *resp, char *note, size_t note_size)
{
    struct kivfs_pkt_reader r;
    const uint8_t *data;
    uint32_t len;
    unsigned records = 0;
    uint64_t ts = clock_next(s);

    int rc = kivfs_pkt_init_response(resp, req, ts, KIVFS_STATUS_OK);
    if (rc != KIVFS_OK)
        return rc;

    kivfs_pkt_reader_init(&r, req);
    while ((rc = kivfs_pkt_read_bytes(&r, &data, &len)) == KIVFS_OK) {
        if ((rc = kivfs_pkt_add_bytes(resp, data, len)) != KIVFS_OK)
            break;
        records++;
    }
    if (rc == KIVFS_EEND) {                       /* přečteno vše = úspěch */
        snprintf(note, note_size, "záznamů: %u", records);
        return KIVFS_OK;
    }

    /* Něco selhalo – rozpracovanou odpověď zahodit. */
    kivfs_pkt_free(resp);
    if (rc == KIVFS_ENOMEM)
        return rc;

    /* EMALFORMED = záznam přetéká payload; ETOOBIG = kopie by přesáhla
     * KIVFS_MAX_PAYLOAD (odpověď je o 12 B větší než požadavek). */
    snprintf(note, note_size, "%s", rc == KIVFS_ETOOBIG ? "odpověď by byla příliš velká"
                                                       : "neplatný payload");
    return kivfs_pkt_init_response(resp, req, ts, KIVFS_STATUS_MALFORMED);
}

static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Jeden PING na peer přes půjčený kanál (líně se připojí). */
static int ping_peer(struct server *s, uint32_t node_id, uint32_t seq)
{
    struct kivfs_peer_link *l;
    int rc = kivfs_peers_acquire(s->peers, node_id, &l);
    if (rc != KIVFS_OK)
        return rc;

    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, KIVFS_REQ_PING, 0);      /* timestamp nastaví link */
    rc = kivfs_pkt_add_u32(&req, seq);
    if (rc == KIVFS_OK)
        rc = kivfs_link_call(l, &req, &resp);
    if (rc == KIVFS_OK) {
        struct kivfs_pkt_reader r;
        uint64_t ref;
        uint32_t echo;
        kivfs_pkt_reader_init(&r, &resp);
        if (resp.header.return_value != KIVFS_STATUS_OK
            || kivfs_pkt_read_u64(&r, &ref) != KIVFS_OK
            || kivfs_pkt_read_u32(&r, &echo) != KIVFS_OK
            || echo != seq)
            rc = KIVFS_EUNEXPECTED;
        kivfs_pkt_free(&resp);
    }
    kivfs_pkt_free(&req);
    kivfs_peers_release(s->peers, l);
    return rc;
}

/* NODES   požadavek: nic
 *         odpověď: [u64 ref] + pro každý ostatní uzel
 *                  [u32 node_id][u32 výsledek][u32 rtt_us][u32 příchozí]
 * Jen klientský worker – volá jiné uzly. */
static int handle_nodes(struct server *s, const struct kivfs_packet *req,
                        struct kivfs_packet *resp, char *note, size_t note_size)
{
    struct kivfs_pkt_reader r;
    kivfs_pkt_reader_init(&r, req);
    if (!kivfs_pkt_reader_at_end(&r)) {
        snprintf(note, note_size, "neplatný payload");
        return kivfs_pkt_init_response(resp, req, clock_next(s), KIVFS_STATUS_MALFORMED);
    }

    /* Pingy proběhnou dřív, než se sestaví odpověď (timestamp odpovědi
     * má být až po nich). Výsledky si zatím schováme. */
    struct kivfs_peer_status st[KIVFS_CFG_MAX_NODES];
    size_t n = kivfs_peers_snapshot(s->peers, st, KIVFS_CFG_MAX_NODES);
    int      result[KIVFS_CFG_MAX_NODES];
    uint32_t rtt[KIVFS_CFG_MAX_NODES];
    unsigned ok = 0;

    for (size_t i = 0; i < n; i++) {
        uint64_t t0 = mono_us();
        result[i] = ping_peer(s, st[i].node_id, (uint32_t)i + 1);
        uint64_t d = mono_us() - t0;
        rtt[i] = d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
        ok += result[i] == KIVFS_OK;
    }
    /* Příchozí stav až po pingech – PING mohl spojení navázat. */
    n = kivfs_peers_snapshot(s->peers, st, KIVFS_CFG_MAX_NODES);

    int rc = kivfs_pkt_init_response(resp, req, clock_next(s), KIVFS_STATUS_OK);
    for (size_t i = 0; rc == KIVFS_OK && i < n; i++) {
        if ((rc = kivfs_pkt_add_u32(resp, st[i].node_id)) == KIVFS_OK
            && (rc = kivfs_pkt_add_u32(resp, (uint32_t)result[i])) == KIVFS_OK
            && (rc = kivfs_pkt_add_u32(resp, rtt[i])) == KIVFS_OK)
            rc = kivfs_pkt_add_u32(resp, (uint32_t)st[i].in_connected);
    }
    if (rc != KIVFS_OK) {
        kivfs_pkt_free(resp);
        return rc;
    }
    snprintf(note, note_size, "dostupných uzlů %u z %zu", ok, n);
    return KIVFS_OK;
}

/* Rozcestník podle request_code (PROVIZORNÍ – nahradí dispatch).
 * Uzlový worker nesmí volat jiné uzly, proto NODES jen na klientském. */
static int handle_request(struct server *s, const struct worker_arg *wa, const struct job *j,
                          struct kivfs_packet *resp, char *note, size_t note_size,
                          struct post_action *post)
{
    const struct kivfs_packet *req = &j->pkt;
    switch (req->header.request_code) {
    case KIVFS_REQ_HELLO: return handle_hello(s, j, resp, note, note_size, post);
    case KIVFS_REQ_PING:  return handle_ping(s, req, resp, note, note_size);
    case KIVFS_REQ_NODES:
        if (!wa->is_node)
            return handle_nodes(s, req, resp, note, note_size);
        break;
    }
    snprintf(note, note_size, "neznámý kód %u", (unsigned)req->header.request_code);
    return kivfs_pkt_init_response(resp, req, clock_next(s), KIVFS_STATUS_UNKNOWN_REQ);
}

/* ------------------------------------------------------------------ */
/* Callbacky reactoru – běží v I/O vlákně, NESMÍ blokovat              */
/* ------------------------------------------------------------------ */

static void on_accept(void *user, kivfs_conn_id id, const char *peer_addr)
{
    (void)user;
    kivfs_log("[io]", "accept conn " KIVFS_CONN_FMT " z %s", KIVFS_CONN_ARGS(id), peer_addr);
}

static void on_packet(void *user, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    struct server *s = user;
    uint16_t code = pkt->header.request_code;

    clock_observe(s, pkt->header.timestamp);

    /* Na příchozím spojení smí chodit jen požadavky. Neodpovědět by
     * nestačilo: reactor až do odpovědi nečte, spojení by zamrzlo. */
    if (kivfs_proto_is_response(code)) {
        kivfs_log("[io]", "conn " KIVFS_CONN_FMT ": přišla odpověď (0x%04x) místo požadavku -> zavírám",
                  KIVFS_CONN_ARGS(id), (unsigned)code);
        kivfs_pkt_free(pkt);
        kivfs_reactor_close_conn(s->reactor, id);
        return;
    }

    /* Do které fronty? Tabulku spojení plní uzlový worker při HELLO, a to
     * PŘED odesláním odpovědi – další paket už tedy vidí správný stav. */
    uint32_t nid;
    enum kivfs_conn_kind kind = kivfs_peers_in_kind(s->peers, id, &nid);
    struct kivfs_queue *q;
    const char *qname;

    if (code == KIVFS_REQ_HELLO) {
        q = s->node_queue;
        qname = "uzlová";
    } else if (kind == KIVFS_CONN_NODE) {
        q = s->node_queue;
        qname = "uzlová";
    } else if (kind == KIVFS_CONN_CLIENT) {
        q = s->client_queue;
        qname = "klientská";
    } else {
        kivfs_log("[io]", "conn " KIVFS_CONN_FMT ": %s před HELLO -> zavírám",
                  KIVFS_CONN_ARGS(id), req_name(code));
        kivfs_pkt_free(pkt);
        kivfs_reactor_close_conn(s->reactor, id);
        return;
    }

    kivfs_log("[io]", "conn " KIVFS_CONN_FMT " -> %s fronta: %s ts=%llu, %u B",
              KIVFS_CONN_ARGS(id), qname, req_name(code),
              (unsigned long long)pkt->header.timestamp, (unsigned)pkt->header.data_size);

    struct job j = { id, nid, *pkt };     /* převzetí vlastnictví paketu */
    if (kivfs_queue_try_push(q, &j) == KIVFS_OK)
        return;

    /* Fronta plná. S kapacitou max_conns a pravidlem "jeden požadavek
     * na spojení" by nastat nemělo – jen obrana. */
    kivfs_log("[io]", "conn " KIVFS_CONN_FMT ": fronta plná -> BUSY", KIVFS_CONN_ARGS(id));
    struct kivfs_packet resp;
    if (kivfs_pkt_init_response(&resp, &j.pkt, clock_next(s), KIVFS_STATUS_BUSY) == KIVFS_OK)
        kivfs_reactor_send(s->reactor, id, &resp);
    else
        kivfs_reactor_close_conn(s->reactor, id);
    kivfs_pkt_free(&j.pkt);
}

static void on_close(void *user, kivfs_conn_id id, enum kivfs_close_reason reason, int err)
{
    struct server *s = user;
    uint32_t nid;
    enum kivfs_conn_kind kind = kivfs_peers_in_kind(s->peers, id, &nid);
    kivfs_peers_in_closed(s->peers, id);

    char who[32] = "";
    if (kind == KIVFS_CONN_NODE)
        snprintf(who, sizeof who, " (uzel %u)", (unsigned)nid);
    else if (kind == KIVFS_CONN_CLIENT)
        snprintf(who, sizeof who, " (klient)");

    if (err != 0)
        kivfs_log("[io]", "conn " KIVFS_CONN_FMT "%s zavřeno: %s (%s)",
                  KIVFS_CONN_ARGS(id), who, close_reason_str(reason), strerror(err));
    else
        kivfs_log("[io]", "conn " KIVFS_CONN_FMT "%s zavřeno: %s",
                  KIVFS_CONN_ARGS(id), who, close_reason_str(reason));
}

/* ------------------------------------------------------------------ */
/* Vlákna                                                              */
/* ------------------------------------------------------------------ */

static void job_free(void *elem)
{
    kivfs_pkt_free(&((struct job *)elem)->pkt);
}

static void *worker_main(void *arg)
{
    struct worker_arg *wa = arg;
    struct server *s = wa->srv;
    char tag[16];
    struct job j;

    snprintf(tag, sizeof tag, "[%c%u]", wa->is_node ? 'n' : 'w', wa->id);

    /* pop čeká, dokud něco nepřijde; po shutdown fronty ještě vydá
     * zbylé joby a pak vrátí KIVFS_ESHUTDOWN. */
    while (kivfs_queue_pop(wa->queue, &j) == KIVFS_OK) {
        struct kivfs_packet resp;
        struct post_action post = { 0, KIVFS_CONN_ID_INVALID };
        char note[128] = "";

        int rc = handle_request(s, wa, &j, &resp, note, sizeof note, &post);
        if (rc != KIVFS_OK) {
            /* Bez odpovědi by spojení navždy čekalo -> zavřít. */
            kivfs_log(tag, "conn " KIVFS_CONN_FMT " %s: %s -> zavírám spojení",
                      KIVFS_CONN_ARGS(j.conn), req_name(j.pkt.header.request_code),
                      kivfs_strerror(rc));
            kivfs_reactor_close_conn(s->reactor, j.conn);
        } else {
            /* Údaje pro log číst PŘED send – send paket vyprázdní. */
            kivfs_log(tag, "conn " KIVFS_CONN_FMT " %s (%s) -> %s, odpověď ts=%llu",
                      KIVFS_CONN_ARGS(j.conn), req_name(j.pkt.header.request_code), note,
                      kivfs_status_str(resp.header.return_value),
                      (unsigned long long)resp.header.timestamp);
            rc = kivfs_reactor_send(s->reactor, j.conn, &resp);
            if (rc != KIVFS_OK)                  /* typicky EIO při vypínání */
                kivfs_log(tag, "conn " KIVFS_CONN_FMT ": odpověď zahozena (%s)",
                          KIVFS_CONN_ARGS(j.conn), kivfs_strerror(rc));
            /* close čeká na už zařazené pakety, takže odpověď odejde. */
            if (post.close_conn)
                kivfs_reactor_close_conn(s->reactor, j.conn);
        }
        if (post.close_other != KIVFS_CONN_ID_INVALID) {
            kivfs_log(tag, "zavírám nahrazené spojení " KIVFS_CONN_FMT,
                      KIVFS_CONN_ARGS(post.close_other));
            kivfs_reactor_close_conn(s->reactor, post.close_other);
        }
        kivfs_pkt_free(&j.pkt);
    }
    kivfs_log(tag, "končím");
    return NULL;
}

static void *io_main(void *arg)
{
    struct server *s = arg;
    int rc = kivfs_reactor_run(s->reactor);
    if (rc != KIVFS_OK) {
        /* Main spí v sigwait – bez signálu by se o chybě nedozvěděl. */
        kivfs_log("[io]", "reactor skončil s chybou: %s", kivfs_strerror(rc));
        kill(getpid(), SIGTERM);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "použití: %s <konfigurační soubor>\n"
                    "  formát souboru viz common/config.h, ukázky v conf/\n", prog);
}

/* Spustí n workerů nad frontou q; vrací počet skutečně spuštěných. */
static unsigned start_workers(struct server *s, struct kivfs_queue *q, int is_node,
                              pthread_t *th, struct worker_arg *args, unsigned n)
{
    unsigned i = 0;
    for (; i < n; i++) {
        args[i] = (struct worker_arg){ s, q, is_node, i };
        if (pthread_create(&th[i], NULL, worker_main, &args[i]) != 0) {
            kivfs_log("[main]", "nelze spustit %s worker %u", is_node ? "uzlový" : "klientský", i);
            break;
        }
    }
    return i;
}

/* Ukončí obě fronty a počká na všechny workery. Klientský worker může
 * zrovna čekat na odpověď jiného uzlu – pak tu main čeká nejvýš
 * PEER_IO_TIMEOUT_MS. */
static void stop_workers(struct server *s, pthread_t *th, unsigned n)
{
    kivfs_queue_shutdown(s->client_queue);
    kivfs_queue_shutdown(s->node_queue);
    for (unsigned i = 0; i < n; i++)
        pthread_join(th[i], NULL);
}

static void destroy_all(struct server *s)
{
    if (s->reactor != NULL)
        kivfs_reactor_destroy(s->reactor);
    if (s->client_queue != NULL)
        kivfs_queue_destroy(s->client_queue);
    if (s->node_queue != NULL)
        kivfs_queue_destroy(s->node_queue);
    if (s->peers != NULL)
        kivfs_peers_destroy(s->peers);
    pthread_mutex_destroy(&s->clock_lock);
}

int main(int argc, char **argv)
{
    /* ---- 1. konfigurace ---- */
    if (argc != 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 2;
    }

    static struct kivfs_config cfg;     /* static: ~17 KB, ať nezabírá zásobník */
    char cfg_err[512];
    int rc = kivfs_config_load(argv[1], &cfg, cfg_err, sizeof cfg_err);
    if (rc != KIVFS_OK) {
        kivfs_log("[main]", "konfigurace: %s", cfg_err);
        return 1;
    }
    const struct kivfs_node_addr *self = kivfs_config_find(&cfg, cfg.self_id);
    unsigned long port = self->port;
    unsigned long max_conns = cfg.max_conns;
    unsigned n_client_workers = cfg.workers;
    unsigned n_node_workers = cfg.n_nodes > 1 ? (unsigned)(cfg.n_nodes - 1) : 1u;

    /* ---- 2. signály – PŘED vytvořením jakéhokoli vlákna ---- */
    kivfs_sock_ignore_sigpipe();
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    /* ---- 3. peery, fronty a reactor ---- */
    struct server srv;
    memset(&srv, 0, sizeof srv);
    srv.cfg = &cfg;
    srv.node_id = cfg.self_id;
    pthread_mutex_init(&srv.clock_lock, NULL);

    struct kivfs_peers_config pc = {
        .cfg = &cfg, .connect_timeout_ms = PEER_CONNECT_TIMEOUT_MS,
        .io_timeout_ms = PEER_IO_TIMEOUT_MS, .backoff_ms = PEER_BACKOFF_MS
    };
    if ((rc = kivfs_peers_create(&srv.peers, &pc)) != KIVFS_OK
        || (rc = kivfs_queue_create(&srv.client_queue, max_conns, sizeof(struct job),
                                    job_free)) != KIVFS_OK
        /* uzlová fronta: od uzlů jen n-1 požadavků, ale HELLO sem posílají
         * i klienti -> stejná kapacita jako klientská */
        || (rc = kivfs_queue_create(&srv.node_queue, max_conns, sizeof(struct job),
                                    job_free)) != KIVFS_OK) {
        kivfs_log("[main]", "inicializace: %s", kivfs_strerror(rc));
        destroy_all(&srv);
        return 1;
    }

    struct kivfs_reactor_config rcfg = {
        .bind_addr = NULL, .port = (uint16_t)port, .backlog = LISTEN_BACKLOG,
        .max_conns = (uint32_t)max_conns, .rx_timeout_ms = RX_TIMEOUT_MS
    };
    struct kivfs_reactor_callbacks cb = {
        .on_accept = on_accept, .on_packet = on_packet, .on_close = on_close, .user = &srv
    };
    rc = kivfs_reactor_create(&srv.reactor, &rcfg, &cb);
    if (rc != KIVFS_OK) {
        int e = errno;
        kivfs_log("[main]", "nelze naslouchat na portu %lu: %s%s%s", port, kivfs_strerror(rc),
                  rc == KIVFS_EIO ? " - " : "", rc == KIVFS_EIO ? strerror(e) : "");
        srv.reactor = NULL;
        destroy_all(&srv);
        return 1;
    }
    kivfs_log("[main]", "konfigurace %s: node_id=%u, port %lu, %u klientských + %u uzlových "
              "workerů, max %lu spojení, otisk seznamu %016llx",
              argv[1], (unsigned)cfg.self_id, port, n_client_workers, n_node_workers, max_conns,
              (unsigned long long)kivfs_config_fingerprint(&cfg));
    for (size_t i = 0; i < cfg.n_nodes; i++)
        kivfs_log("[main]", "  uzel %u = %s:%u%s", (unsigned)cfg.nodes[i].node_id,
                  cfg.nodes[i].host, (unsigned)cfg.nodes[i].port,
                  cfg.nodes[i].node_id == cfg.self_id ? "  (já)" : "");

    /* ---- 4. vlákna ---- */
    static pthread_t workers[2 * KIVFS_CFG_MAX_WORKERS + KIVFS_CFG_MAX_NODES];
    static struct worker_arg wargs[2 * KIVFS_CFG_MAX_WORKERS + KIVFS_CFG_MAX_NODES];
    pthread_t io_thread;
    unsigned started, started_c, started_n;
    int exit_code = 0;

    started_c = start_workers(&srv, srv.client_queue, 0, workers, wargs, n_client_workers);
    started_n = started_c < n_client_workers ? 0
              : start_workers(&srv, srv.node_queue, 1, workers + started_c,
                              wargs + started_c, n_node_workers);
    started = started_c + started_n;
    if (started != n_client_workers + n_node_workers)
        exit_code = 1;

    if (exit_code == 0) {
        kivfs_log("[main]", "spuštěno %u klientských a %u uzlových workerů", started_c, started_n);
        if (pthread_create(&io_thread, NULL, io_main, &srv) != 0) {
            kivfs_log("[main]", "nelze spustit I/O vlákno");
            exit_code = 1;
        }
    }
    if (exit_code != 0) {
        stop_workers(&srv, workers, started);
        destroy_all(&srv);                       /* run neběžel – smí se hned */
        return exit_code;
    }
    kivfs_log("[main]", "spuštěno I/O vlákno");

    /* ---- 5. připojení k peerům – AŽ když reactor a workery běží, jinak
     *         by se dva současně startující uzly navzájem neodbavily ---- */
    kivfs_peers_connect_all(srv.peers);
    kivfs_log("[main]", "úvodní připojení k peerům hotovo, čekám na SIGINT/SIGTERM");

    /* ---- 6. čekání na signál ---- */
    int sig = 0;
    sigwait(&sigs, &sig);

    /* ---- 7. úklid v pořadí podle reactor.h ---- */
    kivfs_log("[main]", "%s -> zastavuji reactor", sig == SIGINT ? "SIGINT" : "SIGTERM");
    kivfs_reactor_stop(srv.reactor);
    pthread_join(io_thread, NULL);
    kivfs_log("[main]", "I/O vlákno skončilo, ukončuji fronty");
    stop_workers(&srv, workers, started);
    kivfs_log("[main]", "workery skončily, konec");

    destroy_all(&srv);
    return 0;
}
