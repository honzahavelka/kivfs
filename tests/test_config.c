/*
 * Unit testy pro config.c (bez sítě, běží i na macOS).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "errors.h"

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

/* Konfigurace musí selhat s KIVFS_ECONFIG a text chyby musí obsahovat
 * `needle` (typicky číslo řádku nebo část popisu). */
#define CHECK_BAD(text, needle)                                            \
    do {                                                                   \
        struct kivfs_config c_;                                            \
        char e_[256] = "";                                                 \
        int rc_ = kivfs_config_parse((text), &c_, e_, sizeof e_);          \
        g_checks++;                                                        \
        if (rc_ != KIVFS_ECONFIG || strstr(e_, (needle)) == NULL) {        \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: rc=%d err='%s', čekal '%s'\n",    \
                    __FILE__, __LINE__, rc_, e_, (needle));                \
        }                                                                  \
    } while (0)

static const char *VALID =
    "# ukázka\n"
    "[node]\n"
    "id        = 2\n"
    "workers   = 8      # komentář za hodnotou\n"
    "max_conns = 100\n"
    "\n"
    "[nodes]\n"
    "3 = node3:30002\n"                 /* záměrně neseřazené */
    "1 = 147.228.67.121:30002\n"
    "  2=147.228.67.122:30003  \n";

static void test_valid(void)
{
    struct kivfs_config c;
    char err[256] = "";
    CHECK(kivfs_config_parse(VALID, &c, err, sizeof err) == KIVFS_OK);
    CHECK(c.self_id == 2);
    CHECK(c.workers == 8);
    CHECK(c.max_conns == 100);
    CHECK(c.n_nodes == 3);
    /* seřazeno podle node_id */
    CHECK(c.nodes[0].node_id == 1 && c.nodes[1].node_id == 2 && c.nodes[2].node_id == 3);
    CHECK(strcmp(c.nodes[0].host, "147.228.67.121") == 0 && c.nodes[0].port == 30002);
    CHECK(strcmp(c.nodes[1].host, "147.228.67.122") == 0 && c.nodes[1].port == 30003);
    CHECK(strcmp(c.nodes[2].host, "node3") == 0);

    const struct kivfs_node_addr *self = kivfs_config_find(&c, c.self_id);
    CHECK(self != NULL && self->port == 30003);
    CHECK(kivfs_config_find(&c, 4) == NULL);
}

static void test_defaults_crlf_single_node(void)
{
    struct kivfs_config c;
    CHECK(kivfs_config_parse("[node]\r\nid=1\r\n[nodes]\r\n1 = 127.0.0.1:9001\r\n",
                             &c, NULL, 0) == KIVFS_OK);
    CHECK(c.workers == KIVFS_CFG_DEFAULT_WORKERS);
    CHECK(c.max_conns == KIVFS_CFG_DEFAULT_MAX_CONNS);
    CHECK(c.n_nodes == 1);                          /* jednouzlový cluster je OK */
    CHECK(strcmp(c.nodes[0].host, "127.0.0.1") == 0);

    /* poslední řádek bez '\n' */
    CHECK(kivfs_config_parse("[nodes]\n5 = a:1\n[node]\nid = 5", &c, NULL, 0) == KIVFS_OK);
    CHECK(c.self_id == 5 && c.nodes[0].port == 1);
}

static void test_syntax_errors(void)
{
    CHECK_BAD("[node]\nid 2\n", "řádek 2");                       /* chybí '=' */
    CHECK_BAD("id = 2\n", "mimo sekci");
    CHECK_BAD("[uzly]\n", "neznámá sekce");
    CHECK_BAD("[node\n", "neuzavřená");
    CHECK_BAD("[node]\nworker = 4\n", "neznámý klíč 'worker'");     /* překlep */
    CHECK_BAD("[node]\n = 4\n", "chybí klíč");
    CHECK_BAD("[node]\nid =   # nic\n", "chybí hodnota");

    /* řádek delší než limit */
    char longline[700];
    memset(longline, 'x', sizeof longline);
    longline[sizeof longline - 1] = '\0';
    CHECK_BAD(longline, "delší");
}

static void test_node_values(void)
{
    const char *nodes = "\n[nodes]\n1 = a:1\n2 = b:2\n";
    char buf[512];

    snprintf(buf, sizeof buf, "[node]\nid = 0%s", nodes);        CHECK_BAD(buf, "neplatné id");
    snprintf(buf, sizeof buf, "[node]\nid = -1%s", nodes);       CHECK_BAD(buf, "neplatné id");
    snprintf(buf, sizeof buf, "[node]\nid = 1x%s", nodes);       CHECK_BAD(buf, "neplatné id");
    snprintf(buf, sizeof buf, "[node]\nid = 1\nid = 2%s", nodes); CHECK_BAD(buf, "dvakrát");
    snprintf(buf, sizeof buf, "[node]\nid = 1\nworkers = 0%s", nodes);   CHECK_BAD(buf, "workers");
    snprintf(buf, sizeof buf, "[node]\nid = 1\nworkers = 65%s", nodes);  CHECK_BAD(buf, "workers");
    snprintf(buf, sizeof buf, "[node]\nid = 1\nmax_conns = 0%s", nodes); CHECK_BAD(buf, "max_conns");
    snprintf(buf, sizeof buf, "[node]\nid = 3%s", nodes);        CHECK_BAD(buf, "není v [nodes]");
    CHECK_BAD("[nodes]\n1 = a:1\n", "chybí [node] id");
    CHECK_BAD("[node]\nid = 1\n", "[nodes]");
}

static void test_nodes_entries(void)
{
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = a:1\n1 = b:2\n", "řádek 5: duplicitní node_id 1");
    CHECK_BAD("[node]\nid=1\n[nodes]\n0 = a:1\n", "neplatné node_id");
    CHECK_BAD("[node]\nid=1\n[nodes]\nx = a:1\n", "neplatné node_id");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = a\n", "host:port");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = :9000\n", "prázdný host");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = a:0\n", "neplatný port");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = a:65536\n", "neplatný port");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = a:\n", "neplatný port");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = ::1:9000\n", "IPv6");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = a b:9000\n", "mezeru");
    CHECK_BAD("[node]\nid=1\n[nodes]\n1 = a:1\n2 = a:1\n", "stejnou adresu");
    CHECK_BAD("[node]\nid=1\nmax_conns=1\n[nodes]\n1 = a:1\n2 = b:1\n", "max_conns");

    /* příliš dlouhý host */
    char buf[600];
    int n = snprintf(buf, sizeof buf, "[node]\nid=1\n[nodes]\n1 = ");
    memset(buf + n, 'h', 300);
    snprintf(buf + n + 300, sizeof buf - (size_t)n - 300, ":1\n");
    CHECK_BAD(buf, "delší");

    /* příliš mnoho uzlů */
    static char many[8192];
    size_t off = (size_t)snprintf(many, sizeof many, "[node]\nid=1\nmax_conns=1000\n[nodes]\n");
    for (int i = 1; i <= KIVFS_CFG_MAX_NODES + 1; i++)
        off += (size_t)snprintf(many + off, sizeof many - off, "%d = h%d:1\n", i, i);
    CHECK_BAD(many, "příliš mnoho");
}

static void test_out_unchanged_on_error(void)
{
    struct kivfs_config c;
    memset(&c, 0xAB, sizeof c);
    CHECK(kivfs_config_parse("[node]\nid = 0\n", &c, NULL, 0) == KIVFS_ECONFIG);
    CHECK(c.self_id == 0xABABABABu);              /* *out se nezměnil */
}

static void test_load(void)
{
    struct kivfs_config c;
    char err[256] = "";

    CHECK(kivfs_config_load("/neexistuje/kivfs.conf", &c, err, sizeof err) == KIVFS_EIO);
    CHECK(strstr(err, "/neexistuje/kivfs.conf") != NULL);

    char path[] = "/tmp/kivfs_test_config_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "w");
    fputs(VALID, f);
    fclose(f);
    CHECK(kivfs_config_load(path, &c, err, sizeof err) == KIVFS_OK);
    CHECK(c.self_id == 2 && c.n_nodes == 3);

    /* chyba ze souboru nese jméno souboru i řádek */
    f = fopen(path, "w");
    fputs("[node]\nid = 1\n[nodes]\n1 = a:1\n1 = a:2\n", f);
    fclose(f);
    CHECK(kivfs_config_load(path, &c, err, sizeof err) == KIVFS_ECONFIG);
    CHECK(strstr(err, path) != NULL && strstr(err, "řádek 5") != NULL);
    unlink(path);
}

int main(void)
{
    test_valid();
    test_defaults_crlf_single_node();
    test_syntax_errors();
    test_node_values();
    test_nodes_entries();
    test_out_unchanged_on_error();
    test_load();

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
