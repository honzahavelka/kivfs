#ifndef KIVFS_MODCONF_H
#define KIVFS_MODCONF_H

/*
 * KIVFS – konfigurace z pohledu jednoho modulu
 * ============================================
 *
 * Jeden INI soubor na UZEL (stejný soubor dostane každý stroj uzlu):
 *
 *     [cluster]
 *     local = node1                ; kdo jsem
 *     node1 = 147.228.67.121       ; adresa SYNC uzlu 1 (port volitelný)
 *     node2 = node2-sync:30102     ; hostname i vlastní port
 *
 *     [auth]  ip = ...  port = ... ; jedna sekce na modul
 *     [sync]  ...
 *     [vfs]   ...
 *     [db]    ...
 *     [fs]    ...
 *
 * Modul (kivfs-<modul> -c soubor.ini) z toho čte:
 *   - [cluster]: node id, adresy Sync všech uzlů,
 *   - svou sekci: ip + port + volitelné klíče (viz níže),
 *   - sekce modulů, které volá (jen ip + port) – kdo koho volá, je pevně
 *     v kódu: Auth -> Sync -> VFS -> {DB, FS}.
 *
 * Klíče sekce modulu:
 *   ip                  POVINNÝ  adresa, na které modul najdou ostatní
 *                                (IP nebo hostname; naslouchá se na všech
 *                                rozhraních, pokud není bind)
 *   port                POVINNÝ  naslouchací port (1..65535)
 *   bind                         naslouchat jen na této adrese
 *   max_conns                    příchozí spojení (výchozí 256)
 *   max_out_conns                odchozí spojení (výchozí reactoru, 64)
 *   max_pending                  požadavky na odchozí spojení (výchozí 16)
 *   connect_timeout_ms           (výchozí 5000)
 *   rx_timeout_ms                max. doba příjmu jednoho paketu (výchozí 10000)
 *   + klíče specifické pro modul (extra_keys), např. mysql_user v [db]
 *
 * Validace – fail fast (KIVFS_ECONFIG, "soubor:řádek: důvod"):
 *   - neznámá sekce (jen cluster, auth, sync, vfs, db, fs),
 *   - neznámý klíč ve VLASTNÍ sekci a v [cluster] (překlep "prot = 30002"
 *     by jinak tiše vzal výchozí hodnotu); cizí sekce kontroluje jejich
 *     vlastní modul při svém startu,
 *   - chybějící povinný klíč, port mimo 1..65535, nečíselná hodnota,
 *   - nodeN: N >= 1 bez úvodních nul (node0, node01 = chyba),
 *     nejvýš KIVFS_MAX_NODES uzlů, local musí odkazovat na existující nodeN,
 *   - chybí sekce modulu, který tento modul volá.
 * Adresy se při načítání NEpřekládají (v Dockeru jména ostatních
 * kontejnerů ještě nemusí existovat) – až při connect (kivfs_modconf_resolve).
 *
 * Co validace nepozná: různé seznamy nodeN na různých uzlech (každý
 * soubor je sám o sobě platný). To odhalí až HELLO (cluster_hash).
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "util/config.h"
#include "proto/protocol.h"
#include "core/reactor.h"

#define KIVFS_MAX_NODES 32
#define KIVFS_HOST_MAX  256      /* včetně '\0'; DNS jméno má max. 253 znaků */

/* Adresa protistrany. Host se překládá až při každém connect – po restartu
 * kontejneru může mít jiné IP. */
struct kivfs_endpoint {
    char     host[KIVFS_HOST_MAX];
    uint16_t port;
};

struct kivfs_node_addr {
    uint32_t              id;        /* N z "nodeN", >= 1 */
    struct kivfs_endpoint sync;      /* Sync modul uzlu */
};

struct kivfs_modconf {
    enum kivfs_module     self;
    uint32_t              local_node;
    struct kivfs_endpoint advertise;                /* moje ip + port */
    char                  bind[KIVFS_HOST_MAX];     /* "" = všechna rozhraní */

    struct kivfs_endpoint deps[KIVFS_MOD_COUNT];    /* moduly, které volám */
    uint8_t               has_dep[KIVFS_MOD_COUNT];

    struct kivfs_node_addr nodes[KIVFS_MAX_NODES];  /* seřazeno podle id */
    size_t                 node_count;

    uint32_t              max_conns;
    uint32_t              max_out_conns;            /* 0 = výchozí reactoru */
    uint32_t              max_pending;              /* 0 = výchozí reactoru */
    int                   connect_timeout_ms;       /* 0 = výchozí reactoru */
    int                   rx_timeout_ms;
};

/* Sestaví pohled modulu self z načtené konfigurace a zvaliduje ji.
 * extra_keys = klíče specifické pro modul povolené v jeho sekci
 * (pole zakončené NULL, smí být NULL); jejich hodnoty si modul přečte
 * sám přes kivfs_config_get. Typické použití:
 *
 *     struct kivfs_config *c;
 *     struct kivfs_modconf mc;
 *     char err[256];
 *     if (kivfs_config_load(path, &c, err, sizeof err) != KIVFS_OK
 *         || kivfs_modconf_from(c, KIVFS_MOD_DB, db_keys, &mc, err, sizeof err) != KIVFS_OK) {
 *         fprintf(stderr, "%s\n", err);
 *         exit(1);
 *     }
 *     const char *user = kivfs_config_get(c, "db", "mysql_user");
 *     ...
 *     kivfs_config_free(c);       // mc je samostatná kopie
 *
 * Vrací KIVFS_OK, KIVFS_ECONFIG nebo KIVFS_EINVAL. */
int  kivfs_modconf_from(const struct kivfs_config *c, enum kivfs_module self,
                        const char *const *extra_keys,
                        struct kivfs_modconf *out, char *err, size_t errlen);

/* Vyplní konfiguraci reactoru z mc. bind_addr ukazuje DO mc – mc musí žít,
 * dokud se nezavolá kivfs_reactor_create / kivfs_ev_create (ty si ho
 * nekopírují, jen použijí). */
void kivfs_modconf_reactor_cfg(const struct kivfs_modconf *mc,
                               struct kivfs_reactor_config *out);

/* Adresa Sync uzlu id, nebo NULL. */
const struct kivfs_node_addr *kivfs_modconf_node(const struct kivfs_modconf *mc, uint32_t id);

/* Přeloží endpoint na adresu pro kivfs_ev_connect / kivfs_reactor_connect.
 * BLOKUJE (DNS) – nevolat v I/O vlákně. KIVFS_OK nebo KIVFS_ERESOLVE. */
int  kivfs_modconf_resolve(const struct kivfs_endpoint *ep,
                           struct sockaddr_storage *out, socklen_t *out_len);

#endif /* KIVFS_MODCONF_H */
