/*
 * Unit testy pro peers.c.
 *   - příchozí polovina (HELLO, tabulka spojení, nahrazení, odmítnutí):
 *     bez sítě, conn_id se vyrábí ručně
 *   - odchozí polovina: jen chování bez protistrany (nedostupný uzel,
 *     backoff, acquire_all nic nedrží). Celé spojení s HELLO ověřuje
 *     běh clusteru (conf/local-node*.conf).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "errors.h"
#include "packet.h"
#include "peers.h"
#include "protocol.h"

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

/* Porty 1 a 2 na localhostu: nic tam neposlouchá (a bez rootu ani
 * nemůže), takže connect skončí hned "connection refused". */
static const char *CFG_TEXT =
    "[node]\n"
    "id = 1\n"
    "max_conns = 8\n"
    "[nodes]\n"
    "1 = 127.0.0.1:9\n"
    "2 = 127.0.0.1:1\n"
    "3 = 127.0.0.1:2\n";

static struct kivfs_config g_cfg;

#define CONN(gen, slot)  ((((kivfs_conn_id)(gen)) << 32) | (kivfs_conn_id)(slot))

static struct kivfs_peers *make_peers(void)
{
    struct kivfs_peers_config pc = {
        .cfg = &g_cfg, .connect_timeout_ms = 500, .io_timeout_ms = 500, .backoff_ms = 300
    };
    struct kivfs_peers *p = NULL;
    CHECK(kivfs_peers_create(&p, &pc) == KIVFS_OK);
    return p;
}

/* Pošle HELLO s daným payloadem; vrátí status odpovědi (nebo 999 při
 * chybě volání) a vyplní *res. Při OK zkontroluje, že odpověď nese moje id. */
static int hello(struct kivfs_peers *p, kivfs_conn_id id, int with_id, uint32_t nid,
                 int with_fp, uint64_t fp, struct kivfs_hello_result *res)
{
    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, KIVFS_REQ_HELLO, 42);
    if (with_id)
        kivfs_pkt_add_u32(&req, nid);
    if (with_fp)
        kivfs_pkt_add_u64(&req, fp);

    int rc = kivfs_peers_in_hello(p, id, &req, 7, &resp, res);
    kivfs_pkt_free(&req);
    if (rc != KIVFS_OK)
        return 999;

    int status = resp.header.return_value;
    uint64_t ref = 0;
    CHECK(kivfs_pkt_response_ref(&resp, &ref) == KIVFS_OK && ref == 42);
    if (status == KIVFS_STATUS_OK) {
        struct kivfs_pkt_reader r;
        uint32_t their = 0;
        kivfs_pkt_reader_init(&r, &resp);
        CHECK(kivfs_pkt_read_u64(&r, &ref) == KIVFS_OK);
        CHECK(kivfs_pkt_read_u32(&r, &their) == KIVFS_OK && their == 1);
        CHECK(kivfs_pkt_reader_at_end(&r));
    }
    CHECK(res->close_after == (status != KIVFS_STATUS_OK));
    kivfs_pkt_free(&resp);
    return status;
}

static int in_connected(struct kivfs_peers *p, uint32_t node_id)
{
    struct kivfs_peer_status st[4];
    size_t n = kivfs_peers_snapshot(p, st, 4);
    for (size_t i = 0; i < n; i++)
        if (st[i].node_id == node_id)
            return st[i].in_connected;
    return -1;
}

static void test_inbound(void)
{
    struct kivfs_peers *p = make_peers();
    struct kivfs_hello_result res;
    uint64_t fp = kivfs_config_fingerprint(&g_cfg);
    uint32_t nid;

    /* nepředstavené spojení */
    CHECK(kivfs_peers_in_kind(p, CONN(1, 0), &nid) == KIVFS_CONN_UNKNOWN);

    /* klient */
    CHECK(hello(p, CONN(1, 0), 1, 0, 0, 0, &res) == KIVFS_STATUS_OK);
    CHECK(kivfs_peers_in_kind(p, CONN(1, 0), &nid) == KIVFS_CONN_CLIENT && nid == 0);
    /* opakované HELLO se stejnou identitou je OK (klientský příkaz hello) */
    CHECK(hello(p, CONN(1, 0), 1, 0, 0, 0, &res) == KIVFS_STATUS_OK);
    /* změna identity ne */
    CHECK(hello(p, CONN(1, 0), 1, 2, 1, fp, &res) == KIVFS_STATUS_MALFORMED);
    CHECK(kivfs_peers_in_kind(p, CONN(1, 0), &nid) == KIVFS_CONN_CLIENT);
    /* stejný slot, jiná generace = jiné spojení, staré se nevidí */
    CHECK(kivfs_peers_in_kind(p, CONN(2, 0), &nid) == KIVFS_CONN_UNKNOWN);

    /* vadné payloady */
    CHECK(hello(p, CONN(1, 1), 0, 0, 0, 0, &res) == KIVFS_STATUS_MALFORMED);   /* prázdný */
    CHECK(hello(p, CONN(1, 1), 1, 2, 0, 0, &res) == KIVFS_STATUS_MALFORMED);   /* uzel bez otisku */
    CHECK(hello(p, CONN(1, 1), 1, 0, 1, fp, &res) == KIVFS_STATUS_MALFORMED);  /* klient s otiskem */
    CHECK(kivfs_peers_in_kind(p, CONN(1, 1), NULL) == KIVFS_CONN_UNKNOWN);

    /* odmítnutí */
    CHECK(hello(p, CONN(1, 1), 1, 1, 1, fp, &res) == KIVFS_STATUS_REJECTED);     /* moje id */
    CHECK(hello(p, CONN(1, 1), 1, 9, 1, fp, &res) == KIVFS_STATUS_REJECTED);     /* neznámé */
    CHECK(hello(p, CONN(1, 1), 1, 2, 1, fp ^ 1, &res) == KIVFS_STATUS_REJECTED); /* jiný seznam */
    CHECK(kivfs_peers_in_kind(p, CONN(1, 1), NULL) == KIVFS_CONN_UNKNOWN);
    CHECK(in_connected(p, 2) == 0);

    /* platný uzel 2 */
    CHECK(hello(p, CONN(1, 1), 1, 2, 1, fp, &res) == KIVFS_STATUS_OK);
    CHECK(res.replaced == KIVFS_CONN_ID_INVALID);
    CHECK(kivfs_peers_in_kind(p, CONN(1, 1), &nid) == KIVFS_CONN_NODE && nid == 2);
    CHECK(in_connected(p, 2) == 1);
    CHECK(in_connected(p, 3) == 0);

    /* uzel 2 znovu na jiném spojení: novější vyhrává */
    CHECK(hello(p, CONN(1, 2), 1, 2, 1, fp, &res) == KIVFS_STATUS_OK);
    CHECK(res.replaced == CONN(1, 1));
    CHECK(kivfs_peers_in_kind(p, CONN(1, 2), &nid) == KIVFS_CONN_NODE && nid == 2);
    /* zavření starého (on_close po close_conn) nesmí smazat nové */
    kivfs_peers_in_closed(p, CONN(1, 1));
    CHECK(kivfs_peers_in_kind(p, CONN(1, 1), NULL) == KIVFS_CONN_UNKNOWN);
    CHECK(in_connected(p, 2) == 1);
    /* zavření nového */
    kivfs_peers_in_closed(p, CONN(1, 2));
    CHECK(in_connected(p, 2) == 0);
    /* neznámé / už zavřené id se ignoruje */
    kivfs_peers_in_closed(p, CONN(7, 2));
    kivfs_peers_in_closed(p, CONN(1, 2));

    /* slot mimo max_conns (porušení záruky reactoru) */
    CHECK(hello(p, CONN(1, 8), 1, 0, 0, 0, &res) == KIVFS_STATUS_INTERNAL);
    CHECK(kivfs_peers_in_kind(p, CONN(1, 8), NULL) == KIVFS_CONN_UNKNOWN);

    kivfs_peers_destroy(p);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void test_outbound_unreachable(void)
{
    struct kivfs_peers *p = make_peers();
    struct kivfs_peer_link *l = (struct kivfs_peer_link *)1;
    struct kivfs_peer_link *all[4];
    size_t n = 99;
    uint32_t failed = 0;

    CHECK(kivfs_peers_acquire(p, 1, &l) == KIVFS_EINVAL && l == NULL);   /* já */
    CHECK(kivfs_peers_acquire(p, 9, &l) == KIVFS_EINVAL && l == NULL);   /* neznámý */

    double t0 = now_ms();
    CHECK(kivfs_peers_acquire(p, 2, &l) == KIVFS_EPEERDOWN && l == NULL);
    CHECK(now_ms() - t0 < 400);                     /* refused = hned, ne timeout */

    struct kivfs_peer_status st[4];
    CHECK(kivfs_peers_snapshot(p, st, 4) == 2);
    CHECK(st[0].node_id == 2 && st[0].out_connected == 0 && st[0].out_last_err != 0);
    CHECK(st[1].node_id == 3 && st[1].out_last_err == 0);   /* 3 ještě nikdo nezkoušel */

    /* všechno, nebo nic; selže první (2), nic se nedrží */
    CHECK(kivfs_peers_acquire_all(p, all, &n, &failed) == KIVFS_EPEERDOWN);
    CHECK(n == 0 && failed == 2);
    /* kdyby acquire_all nechal zamčený mutex, tohle by se zaseklo */
    CHECK(kivfs_peers_acquire(p, 3, &l) == KIVFS_EPEERDOWN);
    CHECK(kivfs_peers_acquire(p, 3, &l) == KIVFS_EPEERDOWN);  /* backoff */

    /* nápověda živosti: HELLO od uzlu 2 -> další acquire zkusí connect,
     * i když backoff běží (zde zase selže, ale nesmí to rozbít stav) */
    struct kivfs_hello_result res;
    CHECK(hello(p, CONN(1, 3), 1, 2, 1, kivfs_config_fingerprint(&g_cfg), &res)
          == KIVFS_STATUS_OK);
    CHECK(kivfs_peers_acquire(p, 2, &l) == KIVFS_EPEERDOWN);

    kivfs_peers_release(p, NULL);                   /* NULL je no-op */
    kivfs_peers_connect_all(p);                     /* jen loguje, nesmí spadnout */
    kivfs_peers_destroy(p);
}

static void test_fingerprint(void)
{
    struct kivfs_config a, b;
    /* stejný seznam, jiné vlastní id a pořadí řádků -> stejný otisk */
    CHECK(kivfs_config_parse("[node]\nid=1\n[nodes]\n1=a:1\n2=b:2\n", &a, NULL, 0) == KIVFS_OK);
    CHECK(kivfs_config_parse("[node]\nid=2\nworkers=8\n[nodes]\n2=b:2\n1=a:1\n", &b, NULL, 0)
          == KIVFS_OK);
    CHECK(kivfs_config_fingerprint(&a) == kivfs_config_fingerprint(&b));
    /* jiný port / host / id -> jiný otisk */
    CHECK(kivfs_config_parse("[node]\nid=1\n[nodes]\n1=a:1\n2=b:3\n", &b, NULL, 0) == KIVFS_OK);
    CHECK(kivfs_config_fingerprint(&a) != kivfs_config_fingerprint(&b));
    CHECK(kivfs_config_parse("[node]\nid=1\n[nodes]\n1=a:1\n2=c:2\n", &b, NULL, 0) == KIVFS_OK);
    CHECK(kivfs_config_fingerprint(&a) != kivfs_config_fingerprint(&b));
    CHECK(kivfs_config_parse("[node]\nid=1\n[nodes]\n1=a:1\n3=b:2\n", &b, NULL, 0) == KIVFS_OK);
    CHECK(kivfs_config_fingerprint(&a) != kivfs_config_fingerprint(&b));
}

int main(void)
{
    char err[256];
    if (kivfs_config_parse(CFG_TEXT, &g_cfg, err, sizeof err) != KIVFS_OK) {
        fprintf(stderr, "testovací konfigurace: %s\n", err);
        return 1;
    }
    test_inbound();
    test_outbound_unreachable();
    test_fingerprint();

    printf("test_peers: %d kontrol, %d selhalo\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
