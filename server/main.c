/*
 * KIVFS – minimální server (jen klient -> server, bez Sync)
 * =========================================================
 *
 * Použití:  kivfs-server --node-id <id> --port <port> [--workers N] [--max-conns N]
 *
 * Vlákna:
 *   main      nastaví vše, pak spí v sigwait; po SIGINT/SIGTERM uklidí
 *   [io]      kivfs_reactor_run: sockety, skládání paketů, odesílání
 *   [w0..wN]  workery: vyzvednou job z fronty, zpracují, odpoví
 *
 * Tok požadavku:
 *   [io] on_packet -> job do fronty -> [wN] handle_request -> kivfs_reactor_send
 *   -> mailbox -> [io] zapíše odpověď do socketu
 *
 * Signály: SIGINT/SIGTERM jsou zablokované ve VŠECH vláknech (maska se
 * nastaví před vytvořením vláken a ta ji zdědí). Main si je vyzvedne
 * synchronně přes sigwait, takže kivfs_reactor_stop se volá z běžného
 * kódu, ne ze signal handleru (tam by zamykání mutexu nebylo bezpečné).
 *
 * PROVIZORNÍ (nahradí další kroky):
 *   - Lamportovy hodiny (clock_*) -> sync.h
 *   - handle_* a switch v handle_request -> rozhraní handlerů (dispatch)
 *   - jedna fronta; uzlová fronta přijde se sync.h (rozlišení podle node_id)
 *   - log vždy zapnutý
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "errors.h"
#include "packet.h"
#include "protocol.h"
#include "queue.h"
#include "reactor.h"
#include "socket.h"

#define DEFAULT_WORKERS    4u
#define MAX_WORKERS        64u
#define DEFAULT_MAX_CONNS  64u
#define MAX_MAX_CONNS      10000u
#define LISTEN_BACKLOG     64
#define RX_TIMEOUT_MS      10000

/* ------------------------------------------------------------------ */
/* Datové struktury                                                    */
/* ------------------------------------------------------------------ */

/* Položka fronty: kdo poslal + co poslal. Fronta položku kopíruje, tím
 * na ni přechází i vlastnictví pkt.data (a při pop zase na workera). */
struct job {
    kivfs_conn_id       conn;
    struct kivfs_packet pkt;
};

/* Celý stav serveru; ukazatel na něj je `user` v callbacích reactoru. */
struct server {
    uint32_t              node_id;
    struct kivfs_reactor *reactor;
    struct kivfs_queue   *queue;

    pthread_mutex_t       clock_lock;   /* PROVIZORNÍ Lamportovy hodiny, */
    uint64_t              clock;        /* přesunou se do sync.h          */
};

struct worker_arg {
    struct server *srv;
    unsigned       id;                  /* jen pro log [wN] */
};

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

/* Jeden řádek logu: "HH:MM:SS.mmm [tag]  text". Řádek se nejdřív složí
 * do bufferu a vypíše jedním fputs, aby se řádky z různých vláken
 * neprokládaly uprostřed. */
static void log_msg(const char *tag, const char *fmt, ...)
{
    char line[512];
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld %-7s ",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L, tag);

    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < sizeof line)
        vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);

    fputs(line, stderr);
    fputc('\n', stderr);
}

/* conn_id do čitelné podoby "generace:slot" (viz reactor.h). */
#define CONN_FMT        "%u:%u"
#define CONN_ARGS(id)   (unsigned)((id) >> 32), (unsigned)((id) & 0xFFFFFFFFu)

static const char *req_name(uint16_t code)
{
    switch (kivfs_proto_base_code(code)) {
    case KIVFS_REQ_HELLO: return "HELLO";
    case KIVFS_REQ_PING:  return "PING";
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

/* HELLO   požadavek: [u32 node_id]   odpověď: [u64 ref][u32 node_id] */
static int handle_hello(struct server *s, const struct kivfs_packet *req,
                        struct kivfs_packet *resp, char *note, size_t note_size)
{
    struct kivfs_pkt_reader r;
    uint32_t peer_id;
    int16_t status = KIVFS_STATUS_OK;

    kivfs_pkt_reader_init(&r, req);
    if (kivfs_pkt_read_u32(&r, &peer_id) != KIVFS_OK || !kivfs_pkt_reader_at_end(&r)) {
        status = KIVFS_STATUS_MALFORMED;
        snprintf(note, note_size, "neplatný payload");
    } else {
        /* Zatím jen informace do logu. Uložení conn_id -> node_id
         * a odmítnutí duplicitního uzlu patří do sync.h. */
        snprintf(note, note_size, "od node_id %u", (unsigned)peer_id);
    }

    int rc = kivfs_pkt_init_response(resp, req, clock_next(s), status);
    if (rc != KIVFS_OK)
        return rc;
    if (status == KIVFS_STATUS_OK && (rc = kivfs_pkt_add_u32(resp, s->node_id)) != KIVFS_OK) {
        kivfs_pkt_free(resp);
        return rc;
    }
    return KIVFS_OK;
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

/* Rozcestník podle request_code (PROVIZORNÍ – nahradí dispatch). */
static int handle_request(struct server *s, const struct kivfs_packet *req,
                          struct kivfs_packet *resp, char *note, size_t note_size)
{
    switch (req->header.request_code) {
    case KIVFS_REQ_HELLO: return handle_hello(s, req, resp, note, note_size);
    case KIVFS_REQ_PING:  return handle_ping(s, req, resp, note, note_size);
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
    log_msg("[io]", "accept conn " CONN_FMT " z %s", CONN_ARGS(id), peer_addr);
}

static void on_packet(void *user, kivfs_conn_id id, struct kivfs_packet *pkt)
{
    struct server *s = user;
    uint16_t code = pkt->header.request_code;

    clock_observe(s, pkt->header.timestamp);

    /* Na příchozím spojení smí chodit jen požadavky. Neodpovědět by
     * nestačilo: reactor až do odpovědi nečte, spojení by zamrzlo. */
    if (kivfs_proto_is_response(code)) {
        log_msg("[io]", "conn " CONN_FMT ": přišla odpověď (0x%04x) místo požadavku -> zavírám",
                CONN_ARGS(id), (unsigned)code);
        kivfs_pkt_free(pkt);
        kivfs_reactor_close_conn(s->reactor, id);
        return;
    }

    log_msg("[io]", "conn " CONN_FMT " -> fronta: %s ts=%llu, %u B",
            CONN_ARGS(id), req_name(code),
            (unsigned long long)pkt->header.timestamp, (unsigned)pkt->header.data_size);

    struct job j = { id, *pkt };          /* převzetí vlastnictví paketu */
    if (kivfs_queue_try_push(s->queue, &j) == KIVFS_OK)
        return;

    /* Fronta plná. S kapacitou max_conns a pravidlem "jeden požadavek
     * na spojení" by nastat nemělo – jen obrana. */
    log_msg("[io]", "conn " CONN_FMT ": fronta plná -> BUSY", CONN_ARGS(id));
    struct kivfs_packet resp;
    if (kivfs_pkt_init_response(&resp, &j.pkt, clock_next(s), KIVFS_STATUS_BUSY) == KIVFS_OK)
        kivfs_reactor_send(s->reactor, id, &resp);
    else
        kivfs_reactor_close_conn(s->reactor, id);
    kivfs_pkt_free(&j.pkt);
}

static void on_close(void *user, kivfs_conn_id id, enum kivfs_close_reason reason, int err)
{
    (void)user;
    if (err != 0)
        log_msg("[io]", "conn " CONN_FMT " zavřeno: %s (%s)",
                CONN_ARGS(id), close_reason_str(reason), strerror(err));
    else
        log_msg("[io]", "conn " CONN_FMT " zavřeno: %s", CONN_ARGS(id), close_reason_str(reason));
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

    snprintf(tag, sizeof tag, "[w%u]", wa->id);

    /* pop čeká, dokud něco nepřijde; po shutdown fronty ještě vydá
     * zbylé joby a pak vrátí KIVFS_ESHUTDOWN. */
    while (kivfs_queue_pop(s->queue, &j) == KIVFS_OK) {
        struct kivfs_packet resp;
        char note[96] = "";

        int rc = handle_request(s, &j.pkt, &resp, note, sizeof note);
        if (rc != KIVFS_OK) {
            /* Bez odpovědi by spojení navždy čekalo -> zavřít. */
            log_msg(tag, "conn " CONN_FMT " %s: %s -> zavírám spojení",
                    CONN_ARGS(j.conn), req_name(j.pkt.header.request_code), kivfs_strerror(rc));
            kivfs_reactor_close_conn(s->reactor, j.conn);
        } else {
            /* Údaje pro log číst PŘED send – send paket vyprázdní. */
            log_msg(tag, "conn " CONN_FMT " %s (%s) -> %s, odpověď ts=%llu",
                    CONN_ARGS(j.conn), req_name(j.pkt.header.request_code), note,
                    kivfs_status_str(resp.header.return_value),
                    (unsigned long long)resp.header.timestamp);
            rc = kivfs_reactor_send(s->reactor, j.conn, &resp);
            if (rc != KIVFS_OK)                  /* typicky EIO při vypínání */
                log_msg(tag, "conn " CONN_FMT ": odpověď zahozena (%s)",
                        CONN_ARGS(j.conn), kivfs_strerror(rc));
        }
        kivfs_pkt_free(&j.pkt);
    }
    log_msg(tag, "končím");
    return NULL;
}

static void *io_main(void *arg)
{
    struct server *s = arg;
    int rc = kivfs_reactor_run(s->reactor);
    if (rc != KIVFS_OK) {
        /* Main spí v sigwait – bez signálu by se o chybě nedozvěděl. */
        log_msg("[io]", "reactor skončil s chybou: %s", kivfs_strerror(rc));
        kill(getpid(), SIGTERM);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int parse_uint(const char *str, unsigned long min, unsigned long max,
                      unsigned long *out)
{
    char *end;
    if (str[0] == '-' || str[0] == '+')
        return -1;
    errno = 0;
    unsigned long v = strtoul(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "použití: %s --node-id <1..4294967295> --port <1..65535>\n"
            "          [--workers <1..%u>] [--max-conns <1..%u>]\n"
            "  výchozí: --workers %u, --max-conns %u\n",
            prog, MAX_WORKERS, MAX_MAX_CONNS, DEFAULT_WORKERS, DEFAULT_MAX_CONNS);
}

/* Ukončí a uvolní workery 0..n-1 (fronta musí existovat). */
static void stop_workers(struct server *s, pthread_t *th, unsigned n)
{
    kivfs_queue_shutdown(s->queue);
    for (unsigned i = 0; i < n; i++)
        pthread_join(th[i], NULL);
}

int main(int argc, char **argv)
{
    unsigned long node_id = 0, port = 0;
    unsigned long n_workers = DEFAULT_WORKERS, max_conns = DEFAULT_MAX_CONNS;

    /* ---- 1. argumenty ---- */
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        int bad;
        if (strcmp(opt, "--node-id") == 0)
            bad = !val || parse_uint(val, 1, UINT32_MAX, &node_id) != 0;
        else if (strcmp(opt, "--port") == 0)
            bad = !val || parse_uint(val, 1, 65535, &port) != 0;
        else if (strcmp(opt, "--workers") == 0)
            bad = !val || parse_uint(val, 1, MAX_WORKERS, &n_workers) != 0;
        else if (strcmp(opt, "--max-conns") == 0)
            bad = !val || parse_uint(val, 1, MAX_MAX_CONNS, &max_conns) != 0;
        else
            bad = 1;
        if (bad) {
            if (strcmp(opt, "--help") != 0)
                fprintf(stderr, "neplatný parametr: %s %s\n", opt, val ? val : "");
            usage(argv[0]);
            return 2;
        }
        i++;                                      /* přeskočit hodnotu */
    }
    if (node_id == 0 || port == 0) {
        usage(argv[0]);
        return 2;
    }

    /* ---- 2. signály – PŘED vytvořením jakéhokoli vlákna ---- */
    kivfs_sock_ignore_sigpipe();
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    /* ---- 3. fronta a reactor ---- */
    struct server srv;
    memset(&srv, 0, sizeof srv);
    srv.node_id = (uint32_t)node_id;
    pthread_mutex_init(&srv.clock_lock, NULL);

    int rc = kivfs_queue_create(&srv.queue, max_conns, sizeof(struct job), job_free);
    if (rc != KIVFS_OK) {
        log_msg("[main]", "fronta: %s", kivfs_strerror(rc));
        return 1;
    }

    struct kivfs_reactor_config cfg = {
        .bind_addr = NULL, .port = (uint16_t)port, .backlog = LISTEN_BACKLOG,
        .max_conns = (uint32_t)max_conns, .rx_timeout_ms = RX_TIMEOUT_MS
    };
    struct kivfs_reactor_callbacks cb = {
        .on_accept = on_accept, .on_packet = on_packet, .on_close = on_close, .user = &srv
    };
    rc = kivfs_reactor_create(&srv.reactor, &cfg, &cb);
    if (rc != KIVFS_OK) {
        int e = errno;
        log_msg("[main]", "nelze naslouchat na portu %lu: %s%s%s", port, kivfs_strerror(rc),
                rc == KIVFS_EIO ? " - " : "", rc == KIVFS_EIO ? strerror(e) : "");
        kivfs_queue_destroy(srv.queue);
        return 1;
    }
    log_msg("[main]", "node_id=%lu, port %lu, %lu workery, max %lu spojení",
            node_id, port, n_workers, max_conns);

    /* ---- 4. vlákna ---- */
    pthread_t workers[MAX_WORKERS];
    struct worker_arg wargs[MAX_WORKERS];
    pthread_t io_thread;
    unsigned started = 0;
    int exit_code = 0;

    for (; started < n_workers; started++) {
        wargs[started].srv = &srv;
        wargs[started].id = started;
        if (pthread_create(&workers[started], NULL, worker_main, &wargs[started]) != 0) {
            log_msg("[main]", "nelze spustit worker %u", started);
            exit_code = 1;
            break;
        }
    }
    if (exit_code == 0) {
        log_msg("[main]", "spuštěno %u workerů", started);
        if (pthread_create(&io_thread, NULL, io_main, &srv) != 0) {
            log_msg("[main]", "nelze spustit I/O vlákno");
            exit_code = 1;
        }
    }
    if (exit_code != 0) {
        stop_workers(&srv, workers, started);
        kivfs_reactor_destroy(srv.reactor);       /* run neběžel – smí se hned */
        kivfs_queue_destroy(srv.queue);
        return exit_code;
    }
    log_msg("[main]", "spuštěno I/O vlákno, čekám na SIGINT/SIGTERM");

    /* ---- 5. čekání na signál ---- */
    int sig = 0;
    sigwait(&sigs, &sig);

    /* ---- 6. úklid v pořadí podle reactor.h ---- */
    log_msg("[main]", "%s -> zastavuji reactor", sig == SIGINT ? "SIGINT" : "SIGTERM");
    kivfs_reactor_stop(srv.reactor);
    pthread_join(io_thread, NULL);
    log_msg("[main]", "I/O vlákno skončilo, ukončuji frontu");
    stop_workers(&srv, workers, started);
    log_msg("[main]", "workery skončily, konec");

    kivfs_reactor_destroy(srv.reactor);
    kivfs_queue_destroy(srv.queue);
    pthread_mutex_destroy(&srv.clock_lock);
    return 0;
}
