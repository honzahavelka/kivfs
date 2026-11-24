/*
 * kivfs-check – ověří, že se cluster propojil podle konfigurace
 * ============================================================
 *
 * Použití:  kivfs-check -c node1.ini -c node2.ini -c node3.ini [--wait S]
 *
 * Z každého souboru vezme adresu Auth daného uzlu, připojí se jako klient
 * (klient smí jen do Auth), pošle HELLO a STATUS. STATUS je kaskáda
 * Auth -> Sync -> VFS -> DB, FS, takže jedno spojení vrátí bloky všech
 * pěti modulů uzlu. Pak porovná, co moduly hlásí, s tím, co má podle
 * konfigurace platit:
 *   - každý uzel: odpovědělo všech 5 modulů
 *   - každý uzel: auth->sync, sync->vfs, vfs->db, vfs->fs jsou UP
 *   - každý pár uzlů: sync->sync jsou UP
 *   - obě strany se shodnou: volaný modul má volajícího v seznamu
 *     příchozích a volající zná jeho aktuální inkarnaci
 *
 * --wait S: opakovat (po 500 ms) až S sekund, dokud není vše OK – po
 * startu clusteru linky chvíli naskakují.
 * Exit: 0 = vše propojeno, 1 = něco chybí, 2 = špatné použití/konfigurace.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "channel.h"
#include "config.h"
#include "diag.h"
#include "errors.h"
#include "hello.h"
#include "modconf.h"
#include "protocol.h"
#include "socket.h"

#define MAX_FILES        KIVFS_MAX_NODES
#define STATUS_TIMEOUT   5000

struct node {
    const char           *file;
    struct kivfs_modconf  mc;        /* pohled Auth: adresa + seznam uzlů */
    int                   reached;   /* STATUS prošel */
    char                  error[400];
    struct kivfs_diag_module mods[KIVFS_MOD_COUNT];
    int                   have[KIVFS_MOD_COUNT];
};

static struct node g_nodes[MAX_FILES];
static int         g_n;

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *mname(uint32_t m)
{
    return kivfs_module_name((enum kivfs_module)m);
}

/* ------------------------------------------------------------------ */
/* Dotaz na jeden uzel                                                 */
/* ------------------------------------------------------------------ */

static void query(struct node *n, uint64_t *clock, uint64_t inc)
{
    struct kivfs_chan ch;
    struct kivfs_packet req, resp;
    const struct kivfs_endpoint *ep = &n->mc.advertise;

    n->reached = 0;
    n->error[0] = '\0';
    memset(n->have, 0, sizeof n->have);

    kivfs_chan_init(&ch);
    int rc = kivfs_chan_open(&ch, ep->host, ep->port, STATUS_TIMEOUT);
    if (rc != KIVFS_OK) {
        snprintf(n->error, sizeof n->error, "Auth %s:%u nedostupný (%s)",
                 ep->host, (unsigned)ep->port, kivfs_strerror(rc));
        return;
    }

    struct kivfs_hello me = { KIVFS_NODE_ID_UNKNOWN, KIVFS_MOD_CLIENT, KIVFS_PROTO_VERSION, inc, 0 };
    struct kivfs_hello srv;
    if (kivfs_hello_request(&req, ++*clock, &me) != KIVFS_OK)
        goto out;
    rc = kivfs_chan_call(&ch, &req, &resp);
    kivfs_pkt_free(&req);
    if (rc != KIVFS_OK) {
        snprintf(n->error, sizeof n->error, "HELLO: %s", kivfs_strerror(rc));
        goto out;
    }
    if (resp.header.return_value != KIVFS_STATUS_OK
        || kivfs_hello_parse_response(&resp, &srv) != KIVFS_OK
        || srv.module != KIVFS_MOD_AUTH || srv.node_id != n->mc.local_node) {
        snprintf(n->error, sizeof n->error, "HELLO odmítnuto nebo odpověděl jiný modul/uzel (%s)",
                 kivfs_status_str(resp.header.return_value));
        kivfs_pkt_free(&resp);
        goto out;
    }
    kivfs_pkt_free(&resp);

    kivfs_pkt_init(&req, KIVFS_REQ_STATUS, ++*clock);
    rc = kivfs_chan_call(&ch, &req, &resp);
    kivfs_pkt_free(&req);
    if (rc != KIVFS_OK || resp.header.return_value != KIVFS_STATUS_OK) {
        snprintf(n->error, sizeof n->error, "STATUS: %s",
                 rc != KIVFS_OK ? kivfs_strerror(rc) : kivfs_status_str(resp.header.return_value));
        if (rc == KIVFS_OK)
            kivfs_pkt_free(&resp);
        goto out;
    }

    struct kivfs_pkt_reader rd;
    struct kivfs_diag_module d;
    uint64_t ref;
    kivfs_pkt_reader_init(&rd, &resp);
    kivfs_pkt_read_u64(&rd, &ref);
    while ((rc = kivfs_diag_read(&rd, &d)) == KIVFS_OK) {
        if (d.module < KIVFS_MOD_COUNT && d.node == n->mc.local_node) {
            n->mods[d.module] = d;
            n->have[d.module] = 1;
        }
    }
    if (rc != KIVFS_EEND)
        snprintf(n->error, sizeof n->error, "STATUS: poškozená odpověď");
    else
        n->reached = 1;
    kivfs_pkt_free(&resp);
out:
    kivfs_chan_close(&ch);
}

/* ------------------------------------------------------------------ */
/* Vyhodnocení                                                         */
/* ------------------------------------------------------------------ */

static struct node *node_by_id(uint32_t id)
{
    for (int i = 0; i < g_n; i++)
        if (g_nodes[i].mc.local_node == id)
            return &g_nodes[i];
    return NULL;
}

static const struct kivfs_diag_module *block(uint32_t node, uint32_t module)
{
    struct node *n = node_by_id(node);
    return (n != NULL && module < KIVFS_MOD_COUNT && n->have[module]) ? &n->mods[module] : NULL;
}

/* Jedna očekávaná linka from@fn -> to@tn. Vrátí 1 = OK. Vypíše řádek. */
static int check_link(uint32_t from, uint32_t fn, uint32_t to, uint32_t tn, int print)
{
    const struct kivfs_diag_module *a = block(fn, from);
    const struct kivfs_diag_module *b = block(tn, to);
    const char *state = "?";
    const char *verdict;
    int ok = 0;

    const struct kivfs_diag_link *l = NULL;
    if (a != NULL)
        for (uint32_t i = 0; i < a->n_out; i++)
            if (a->out[i].module == to && a->out[i].node == tn)
                l = &a->out[i];
    if (l != NULL)
        state = kivfs_link_state_str(l->state);

    int back = 0;
    if (b != NULL)
        for (uint32_t i = 0; i < b->n_in; i++)
            if (b->in[i].module == from && b->in[i].node == fn)
                back = 1;

    if (a == NULL)
        verdict = "volající neodpověděl";
    else if (l == NULL)
        verdict = "volající linku nezná (konfigurace?)";
    else if (l->state != KIVFS_LINK_UP)
        verdict = "linka není nahoře";
    else if (b == NULL)
        verdict = "volaný neodpověděl";
    else if (!back)
        verdict = "volaný o spojení neví";
    else if (l->peer_incarnation != b->incarnation)
        verdict = "nesedí inkarnace (restart?)";
    else {
        verdict = "OK";
        ok = 1;
    }
    if (print) {
        char left[32], right[32];
        snprintf(left, sizeof left, "%s@node%u", mname(from), (unsigned)fn);
        snprintf(right, sizeof right, "%s@node%u", mname(to), (unsigned)tn);
        printf("  %-12s -> %-12s %-10s %s\n", left, right, state, verdict);
    }
    return ok;
}

/* Vrátí 1 = vše OK. */
static int evaluate(int print)
{
    int mods_ok = 0, mods_total = 0, links_ok = 0, links_total = 0;

    for (int i = 0; i < g_n; i++) {
        struct node *n = &g_nodes[i];
        uint32_t id = n->mc.local_node;
        if (print) {
            printf("node%u (%s)\n", (unsigned)id, n->file);
            if (!n->reached)
                printf("  !! %s\n", n->error);
        }
        for (int m = KIVFS_MOD_AUTH; m < KIVFS_MOD_COUNT; m++) {
            mods_total++;
            if (n->have[m]) {
                mods_ok++;
                if (print)
                    printf("  %-5s inkarnace %016llx  %s\n", mname((uint32_t)m),
                           (unsigned long long)n->mods[m].incarnation,
                           n->mods[m].ready ? "připraven" : "NEPŘIPRAVEN");
            } else if (print) {
                printf("  %-5s neodpověděl\n", mname((uint32_t)m));
            }
        }
        links_total += 4;
        links_ok += check_link(KIVFS_MOD_AUTH, id, KIVFS_MOD_SYNC, id, print);
        links_ok += check_link(KIVFS_MOD_SYNC, id, KIVFS_MOD_VFS, id, print);
        links_ok += check_link(KIVFS_MOD_VFS, id, KIVFS_MOD_DB, id, print);
        links_ok += check_link(KIVFS_MOD_VFS, id, KIVFS_MOD_FS, id, print);
        for (int j = 0; j < g_n; j++) {
            uint32_t other = g_nodes[j].mc.local_node;
            if (other == id)
                continue;
            links_total++;
            links_ok += check_link(KIVFS_MOD_SYNC, id, KIVFS_MOD_SYNC, other, print);
        }
    }
    int ok = (mods_ok == mods_total && links_ok == links_total);
    if (print)
        printf("\nVÝSLEDEK: moduly %d/%d, spojení %d/%d – %s\n",
               mods_ok, mods_total, links_ok, links_total, ok ? "OK" : "CHYBA");
    return ok;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int load(struct node *n, const char *file)
{
    struct kivfs_config *c;
    char err[512];
    n->file = file;
    if (kivfs_config_load(file, &c, err, sizeof err) != KIVFS_OK
        || kivfs_modconf_from(c, KIVFS_MOD_AUTH, NULL, &n->mc, err, sizeof err) != KIVFS_OK) {
        fprintf(stderr, "kivfs-check: %s\n", err);
        return -1;
    }
    kivfs_config_free(c);
    return 0;
}

/* Soubory musí popisovat stejný cluster: stejné uzly, každý uzel jednou,
 * a pro každý uzel z [cluster] musí být soubor. */
static int check_files(void)
{
    const struct kivfs_modconf *ref = &g_nodes[0].mc;
    for (int i = 0; i < g_n; i++) {
        const struct kivfs_modconf *mc = &g_nodes[i].mc;
        if (mc->node_count != ref->node_count)
            goto differ;
        for (size_t k = 0; k < mc->node_count; k++)
            if (mc->nodes[k].id != ref->nodes[k].id)
                goto differ;
        for (int j = 0; j < i; j++)
            if (g_nodes[j].mc.local_node == mc->local_node) {
                fprintf(stderr, "kivfs-check: %s a %s jsou oba node%u\n",
                        g_nodes[j].file, g_nodes[i].file, (unsigned)mc->local_node);
                return -1;
            }
    }
    if ((size_t)g_n != ref->node_count) {
        fprintf(stderr, "kivfs-check: [cluster] má %zu uzlů, zadáno %d souborů\n",
                ref->node_count, g_n);
        return -1;
    }
    return 0;
differ:
    fprintf(stderr, "kivfs-check: soubory mají různé seznamy uzlů v [cluster]\n");
    return -1;
}

int main(int argc, char **argv)
{
    long wait_s = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc && g_n < MAX_FILES) {
            if (load(&g_nodes[g_n], argv[++i]) != 0)
                return 2;
            g_n++;
        } else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            wait_s = strtol(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "použití: %s -c node1.ini [-c node2.ini ...] [--wait sekundy]\n", argv[0]);
            return 2;
        }
    }
    if (g_n == 0 || check_files() != 0) {
        if (g_n == 0)
            fprintf(stderr, "použití: %s -c node1.ini [-c node2.ini ...] [--wait sekundy]\n", argv[0]);
        return 2;
    }

    kivfs_sock_ignore_sigpipe();
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint64_t clock = 0, inc = kivfs_incarnation_new();
    long long deadline = now_ms() + wait_s * 1000;
    int ok;
    for (;;) {
        for (int i = 0; i < g_n; i++)
            query(&g_nodes[i], &clock, inc);
        ok = evaluate(0);
        if (ok || now_ms() >= deadline)
            break;
        sleep_ms(500);
    }
    printf("=== KIVFS: propojení clusteru (%d uzly) ===\n", g_n);
    evaluate(1);
    return ok ? 0 : 1;
}
