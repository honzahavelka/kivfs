/*
 * KIVFS – CLI klient (interaktivní)
 * =================================
 *
 * Použití:  kivfs-client <host> <port>
 *
 * Po startu se připojí k modulu Auth uzlu (klient smí JEN do Auth – viz
 * hello.h), pošle HELLO (klient = node_id 0, modul CLIENT) a pak čte
 * příkazy ze stdin, jeden na řádek:
 *
 *     status       stav modulů uzlu a jejich propojení (STATUS přes Auth)
 *     ping [N]     N pingů (1..1000, výchozí 1), vypíše RTT
 *     help         seznam příkazů
 *     quit, exit   konec (stejně jako Ctrl-D)
 *
 * Protože čte stdin po řádcích, funguje i neinteraktivně:
 *     printf "ping 5\nquit\n" | kivfs-client localhost 9000
 * (prompt "kivfs> " se pak nevypisuje).
 *
 * Exit kódy: 0 = konec příkazem/EOF, 1 = chyba spojení, 2 = špatné použití.
 *
 * Chyby:
 *   - chyba kanálu (spojení spadlo, timeout, ...) = kanál je mrtvý (viz
 *     channel.h) -> vypsat a skončit s 1. Bez reconnectu.
 *   - status != OK v odpovědi = server požadavek odmítl, spojení je
 *     v pořádku -> vypsat a pokračovat.
 *
 * Lamportovy hodiny: klient není uzel, timestamp je jen čítač, který se
 * zvýší před každým odesláním. Stačí, aby byl v rámci spojení jedinečný
 * (podle něj se páruje odpověď – viz kivfs_chan_call).
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "net/channel.h"
#include "proto/diag.h"
#include "proto/errors.h"
#include "proto/hello.h"
#include "proto/packet.h"
#include "proto/protocol.h"
#include "net/socket.h"

#define LINE_MAX_LEN   256     /* včetně '\n' a '\0' */
#define MAX_ARGS       8
#define PING_MAX_COUNT 1000

/* Stav jednoho připojení klienta. ID serveru z HELLO je v ch.peer_node_id. */
struct client {
    struct kivfs_chan ch;
    uint64_t clock;            /* čítač timestampů: ++ před každým odesláním */
    uint64_t incarnation;      /* pro HELLO; u klienta jen informativní */
};

/* Výsledek příkazu – říká smyčce REPL, co dál. */
enum cmd_result {
    CMD_OK,      /* pokračovat (i když server vrátil chybový status) */
    CMD_QUIT,    /* uživatel chce skončit */
    CMD_FATAL    /* kanál je mrtvý -> konec s exit kódem 1 */
};

/* ------------------------------------------------------------------ */
/* Pomocné funkce                                                      */
/* ------------------------------------------------------------------ */

static uint64_t next_ts(struct client *c)
{
    return ++c->clock;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Vypíše lokální chybu; u KIVFS_EIO přidá i systémový důvod z errno
 * (např. "Connection refused"). err_no se předává zvlášť, protože errno
 * může přepsat cokoli, co se zavolá mezi chybou a výpisem. */
static void print_error(const char *what, int rc, int err_no)
{
    if (rc == KIVFS_EIO && err_no != 0)
        fprintf(stderr, "%s: %s (%s)\n", what, kivfs_strerror(rc), strerror(err_no));
    else
        fprintf(stderr, "%s: %s\n", what, kivfs_strerror(rc));
}

/* Požadavek -> odpověď. Při chybě kanálu ji vypíše a kanál zavře
 * (je mrtvý). Vrací KIVFS_OK nebo kód chyby z errors.h. */
static int do_call(struct client *c, const struct kivfs_packet *req,
                   struct kivfs_packet *resp)
{
    int rc = kivfs_chan_call(&c->ch, req, resp);
    if (rc != KIVFS_OK) {
        int e = errno;
        char what[KIVFS_SOCK_ADDR_STRLEN + 32];
        snprintf(what, sizeof what, "spojení s %s", c->ch.peer_addr);
        print_error(what, rc, e);
        kivfs_chan_close(&c->ch);
    }
    return rc;
}

/* Rozdělí řádek podle bílých znaků. Mění line (vkládá '\0').
 * Vrací počet argumentů, nebo -1, pokud jich je víc než max_args. */
static int split_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    for (char *tok = strtok(line, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n")) {
        if (argc == max_args)
            return -1;
        argv[argc++] = tok;
    }
    return argc;
}

/* Číslo v rozsahu [min, max] z řetězce; celý řetězec musí být číslo.
 * Vrací 0 = OK, -1 = neplatné. */
static int parse_uint(const char *s, unsigned long min, unsigned long max,
                      unsigned long *out)
{
    char *end;
    if (s[0] == '-' || s[0] == '+')     /* strtoul by "-1" tiše převedl na obří číslo */
        return -1;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* HELLO                                                               */
/* ------------------------------------------------------------------ */

/* Pošle HELLO a zpracuje odpověď. Při úspěchu uloží ID serveru do
 * ch.peer_node_id a nastaví *ok = 1. Odmítnutí nebo nesmyslná odpověď
 * -> *ok = 0, ale spojení je v pořádku (CMD_OK). */
static enum cmd_result do_hello(struct client *c, int *ok)
{
    struct kivfs_packet req, resp;
    struct kivfs_hello me = { KIVFS_NODE_ID_UNKNOWN, KIVFS_MOD_CLIENT, KIVFS_PROTO_VERSION,
                              c->incarnation, 0 };
    struct kivfs_hello srv;

    *ok = 0;
    if (kivfs_hello_request(&req, next_ts(c), &me) != KIVFS_OK) {
        fprintf(stderr, "hello: nedostatek paměti\n");
        return CMD_FATAL;
    }

    int rc = do_call(c, &req, &resp);
    kivfs_pkt_free(&req);
    if (rc != KIVFS_OK)
        return CMD_FATAL;

    if (resp.header.return_value != KIVFS_STATUS_OK) {
        fprintf(stderr, "hello: server odmítl, status %s (klient se smí připojit jen k Auth)\n",
                kivfs_status_str(resp.header.return_value));
    } else if (kivfs_hello_parse_response(&resp, &srv) != KIVFS_OK) {
        fprintf(stderr, "hello: neplatný formát odpovědi\n");
    } else if (srv.module != KIVFS_MOD_AUTH || srv.node_id == KIVFS_NODE_ID_UNKNOWN) {
        fprintf(stderr, "hello: na druhé straně není modul Auth\n");
    } else {
        c->ch.peer_node_id = srv.node_id;
        *ok = 1;
    }
    kivfs_pkt_free(&resp);
    return CMD_OK;
}

/* ------------------------------------------------------------------ */
/* Příkazy                                                             */
/* ------------------------------------------------------------------ */

/* STATUS: Auth se zeptá Syncu, ten VFS a VFS DB a FS – vrátí blok
 * každého modulu uzlu, který odpověděl. (HELLO se znovu poslat nedá:
 * smí být jen jednou, na začátku spojení.) */
static enum cmd_result cmd_status(struct client *c, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "použití: status\n");
        return CMD_OK;
    }
    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, KIVFS_REQ_STATUS, next_ts(c));
    int rc = do_call(c, &req, &resp);
    kivfs_pkt_free(&req);
    if (rc != KIVFS_OK)
        return CMD_FATAL;
    if (resp.header.return_value != KIVFS_STATUS_OK) {
        fprintf(stderr, "status: %s\n", kivfs_status_str(resp.header.return_value));
        kivfs_pkt_free(&resp);
        return CMD_OK;
    }

    struct kivfs_pkt_reader r;
    struct kivfs_diag_module d;
    uint64_t ref;
    kivfs_pkt_reader_init(&r, &resp);
    kivfs_pkt_read_u64(&r, &ref);
    while ((rc = kivfs_diag_read(&r, &d)) == KIVFS_OK) {
        printf("%s@node%u  %s\n", kivfs_module_name(d.module), (unsigned)d.node,
               d.ready ? "připraven" : "NEPŘIPRAVEN");
        for (uint32_t i = 0; i < d.n_out; i++)
            printf("    -> %s@node%u %s\n", kivfs_module_name(d.out[i].module),
                   (unsigned)d.out[i].node, kivfs_link_state_str(d.out[i].state));
    }
    if (rc != KIVFS_EEND)
        fprintf(stderr, "status: poškozená odpověď\n");
    kivfs_pkt_free(&resp);
    return CMD_OK;
}

static enum cmd_result ping_once(struct client *c, uint32_t seq, int *ok, double *rtt_ms)
{
    struct kivfs_packet req, resp;
    struct kivfs_pkt_reader r;
    uint64_t ref;
    uint32_t echo;

    *ok = 0;
    kivfs_pkt_init(&req, KIVFS_REQ_PING, next_ts(c));
    if (kivfs_pkt_add_u32(&req, seq) != KIVFS_OK) {
        fprintf(stderr, "ping: nedostatek paměti\n");
        kivfs_pkt_free(&req);
        return CMD_FATAL;
    }

    double t0 = now_ms();
    int rc = do_call(c, &req, &resp);
    double t1 = now_ms();
    kivfs_pkt_free(&req);
    if (rc != KIVFS_OK)
        return CMD_FATAL;

    if (resp.header.return_value != KIVFS_STATUS_OK) {
        printf("ping %u: status %s\n", (unsigned)seq,
               kivfs_status_str(resp.header.return_value));
    } else {
        kivfs_pkt_reader_init(&r, &resp);
        if (kivfs_pkt_read_u64(&r, &ref) != KIVFS_OK
            || kivfs_pkt_read_u32(&r, &echo) != KIVFS_OK
            || !kivfs_pkt_reader_at_end(&r)) {
            printf("ping %u: neplatný formát odpovědi\n", (unsigned)seq);
        } else if (echo != seq) {
            printf("ping %u: neshoda, server vrátil seq %u\n",
                   (unsigned)seq, (unsigned)echo);
        } else {
            *ok = 1;
            *rtt_ms = t1 - t0;
            printf("ping %u: OK, %.3f ms\n", (unsigned)seq, *rtt_ms);
        }
    }
    kivfs_pkt_free(&resp);
    return CMD_OK;
}

static enum cmd_result cmd_ping(struct client *c, int argc, char **argv)
{
    unsigned long count = 1;
    if (argc > 2 || (argc == 2 && parse_uint(argv[1], 1, PING_MAX_COUNT, &count) != 0)) {
        fprintf(stderr, "použití: ping [N]   (N = 1..%d)\n", PING_MAX_COUNT);
        return CMD_OK;
    }

    unsigned long sent = 0, okcnt = 0;
    double min = 0, max = 0, sum = 0;
    enum cmd_result res = CMD_OK;

    for (unsigned long i = 1; i <= count; i++) {
        int ok;
        double rtt = 0;
        res = ping_once(c, (uint32_t)i, &ok, &rtt);
        if (res == CMD_FATAL)
            break;
        sent++;
        if (ok) {
            if (okcnt == 0 || rtt < min) min = rtt;
            if (okcnt == 0 || rtt > max) max = rtt;
            sum += rtt;
            okcnt++;
        }
    }

    if (res == CMD_FATAL)
        printf("spojení ztraceno po %lu/%lu pingech\n", sent, count);
    if (okcnt > 0)
        printf("%lu/%lu odpovědí OK, rtt min/avg/max = %.3f/%.3f/%.3f ms\n",
               okcnt, count, min, sum / (double)okcnt, max);
    else if (res != CMD_FATAL)
        printf("0/%lu odpovědí OK\n", count);
    return res;
}

static enum cmd_result cmd_help(struct client *c, int argc, char **argv);

static enum cmd_result cmd_quit(struct client *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    return CMD_QUIT;
}

/* Tabulka příkazů – malá obdoba budoucího dispatch na serveru. */
struct command {
    const char *name;
    enum cmd_result (*fn)(struct client *c, int argc, char **argv);
    const char *help;
};

static const struct command COMMANDS[] = {
    { "status", cmd_status, "status       stav modulů uzlu a jejich propojení" },
    { "ping",  cmd_ping,  "ping [N]     N pingů (1..1000, výchozí 1), vypíše RTT" },
    { "help",  cmd_help,  "help         tento přehled" },
    { "quit",  cmd_quit,  "quit, exit   konec (také Ctrl-D)" },
    { "exit",  cmd_quit,  NULL },       /* alias, v nápovědě se nevypisuje */
};
#define N_COMMANDS (sizeof COMMANDS / sizeof COMMANDS[0])

static enum cmd_result cmd_help(struct client *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    for (size_t i = 0; i < N_COMMANDS; i++)
        if (COMMANDS[i].help != NULL)
            printf("  %s\n", COMMANDS[i].help);
    return CMD_OK;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int client_connect(struct client *c, const char *host, uint16_t port)
{
    kivfs_chan_init(&c->ch);
    c->clock = 0;
    c->incarnation = kivfs_incarnation_new();

    int rc = kivfs_chan_open(&c->ch, host, port, KIVFS_CHAN_DEFAULT_TIMEOUT_MS);
    if (rc != KIVFS_OK) {
        int e = errno;
        char what[300];
        snprintf(what, sizeof what, "nelze se připojit k %s:%u", host, (unsigned)port);
        print_error(what, rc, e);
        return -1;
    }

    /* Po připojení vždy nejdřív HELLO. */
    int ok;
    if (do_hello(c, &ok) == CMD_FATAL || !ok) {
        kivfs_chan_close(&c->ch);
        return -1;
    }
    printf("připojeno k %s, server node_id = %u\n",
           c->ch.peer_addr, (unsigned)c->ch.peer_node_id);
    return 0;
}

/* Smyčka čtení příkazů. Vrací exit kód programu. */
static int repl(struct client *c)
{
    char line[LINE_MAX_LEN];
    char *argv[MAX_ARGS];
    int interactive = isatty(STDIN_FILENO);

    for (;;) {
        if (interactive) {
            printf("kivfs> ");
            fflush(stdout);
        }
        if (fgets(line, sizeof line, stdin) == NULL) {   /* EOF (Ctrl-D) nebo chyba */
            if (interactive)
                printf("\n");
            return 0;
        }

        /* Řádek delší než buffer: zbytek zahodit, jinak by se četl jako
         * další příkaz. (Poslední řádek bez '\n' před EOF je v pořádku.) */
        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) { }
            fprintf(stderr, "příliš dlouhý řádek (max %d znaků)\n", LINE_MAX_LEN - 2);
            continue;
        }

        int argc = split_line(line, argv, MAX_ARGS);
        if (argc < 0) {
            fprintf(stderr, "příliš mnoho argumentů (max %d)\n", MAX_ARGS - 1);
            continue;
        }
        if (argc == 0)
            continue;                                    /* prázdný řádek */

        const struct command *cmd = NULL;
        for (size_t i = 0; i < N_COMMANDS; i++)
            if (strcmp(argv[0], COMMANDS[i].name) == 0)
                cmd = &COMMANDS[i];
        if (cmd == NULL) {
            fprintf(stderr, "neznámý příkaz '%s', zkus help\n", argv[0]);
            continue;
        }

        switch (cmd->fn(c, argc, argv)) {
        case CMD_OK:    break;
        case CMD_QUIT:  return 0;
        case CMD_FATAL: return 1;
        }
    }
}

int main(int argc, char **argv)
{
    unsigned long port;
    if (argc != 3 || parse_uint(argv[2], 1, 65535, &port) != 0) {
        fprintf(stderr, "použití: %s <host> <port>\n", argv[0]);
        return 2;
    }

    /* Pojistka: zápis do zavřeného spojení nesmí proces zabít signálem.
     * (socket.c to řeší i per-volání, tohle chrání i zbytek programu.) */
    kivfs_sock_ignore_sigpipe();

    /* Řádkové bufferování i do roury/souboru: jinak by se výpisy na stdout
     * (bufferované po blocích) a stderr (nebufferované) promíchaly. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct client c;
    if (client_connect(&c, argv[1], (uint16_t)port) != 0)
        return 1;

    int code = repl(&c);
    kivfs_chan_close(&c.ch);
    return code;
}
