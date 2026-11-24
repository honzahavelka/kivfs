#ifndef KIVFS_CONFIG_H
#define KIVFS_CONFIG_H

/*
 * KIVFS – konfigurace uzlu ze souboru
 * ===================================
 *
 * Server dostane jediný argument: cestu ke konfiguračnímu souboru.
 * Každý uzel má vlastní soubor; soubory se liší jen v [node] id,
 * seznam [nodes] musí být na všech uzlech stejný.
 *
 *     # kivfs-node2.conf
 *     [node]
 *     id        = 2          # povinné; kdo jsem
 *     workers   = 4          # nepovinné (1..64, výchozí 4)
 *     max_conns = 64         # nepovinné (1..10000, výchozí 64)
 *
 *     [nodes]                # celý cluster VČETNĚ mě, jeden uzel na řádek
 *     1 = 147.228.67.121:30002
 *     2 = 147.228.67.122:30002
 *     3 = 147.228.67.123:30002
 *
 * Port, na kterém server poslouchá, je port z řádku s vlastním id.
 *
 * Syntaxe:
 *   - '#' začíná komentář až do konce řádku; prázdné řádky se ignorují
 *   - mezery a tabulátory kolem sekcí, klíčů a hodnot se ignorují; CRLF je OK
 *   - neznámá sekce nebo klíč = chyba (překlep se nesmí ztratit potichu)
 *   - host je IPv4 adresa nebo jméno (v Dockeru jméno služby); IPv6 zatím ne.
 *     Jméno se NEpřekládá při načtení, ale až při connect – v Dockeru nemusí
 *     být jména ostatních kontejnerů dostupná hned po startu.
 *
 * Validace (cokoli z toho -> KIVFS_ECONFIG s popisem a číslem řádku):
 *   - id chybí, je 0, nebo není v [nodes]
 *   - v [nodes]: id 0, duplicitní id, chybí ":port", port mimo 1..65535,
 *     prázdný/dlouhý host, víc než KIVFS_CFG_MAX_NODES uzlů
 *   - dvě id na stejné adrese host:port
 *   - workers / max_conns mimo rozsah
 *   - max_conns < počet uzlů: ostatní uzly by se nevešly do slotů reactoru
 *
 * Cluster s jediným uzlem je povolený (užitečné pro testy).
 *
 * Omezení: členství je statické. Přidání uzlu = změna souborů a restart
 * všech uzlů. Shodu seznamů mezi uzly ověří až HELLO (otisk seznamu).
 *
 * Struktura nemá dynamickou paměť – nic se neuvolňuje.
 * Návratové kódy: errors.h.
 */

#include <stddef.h>
#include <stdint.h>

#define KIVFS_CFG_MAX_NODES          64
#define KIVFS_CFG_HOST_MAX           256      /* včetně '\0' */

#define KIVFS_CFG_DEFAULT_WORKERS    4u
#define KIVFS_CFG_MAX_WORKERS        64u
#define KIVFS_CFG_DEFAULT_MAX_CONNS  64u
#define KIVFS_CFG_MAX_MAX_CONNS      10000u

/* Adresa jednoho uzlu clusteru. */
struct kivfs_node_addr {
    uint32_t node_id;                      /* >= 1 */
    char     host[KIVFS_CFG_HOST_MAX];
    uint16_t port;
};

struct kivfs_config {
    uint32_t self_id;
    unsigned workers;
    uint32_t max_conns;
    size_t   n_nodes;                                  /* včetně sebe, >= 1 */
    struct kivfs_node_addr nodes[KIVFS_CFG_MAX_NODES]; /* SEŘAZENO vzestupně podle
                                                        * node_id (hodí se pro
                                                        * zamykání podle node_id) */
};

/* Načte a zvaliduje soubor. Při chybě vyplní err čitelnou zprávou
 * ("kivfs-node2.conf: řádek 9: duplicitní node_id 2") a *out NEZMĚNÍ.
 * err smí být NULL. Vrací KIVFS_OK, KIVFS_EIO (soubor nejde otevřít
 * nebo přečíst; důvod je v err) nebo KIVFS_ECONFIG. */
int kivfs_config_load(const char *path, struct kivfs_config *out,
                      char *err, size_t err_size);

/* Totéž z řetězce v paměti (kvůli testům). Chyby jsou ve tvaru
 * "řádek 9: ...". */
int kivfs_config_parse(const char *text, struct kivfs_config *out,
                       char *err, size_t err_size);

/* Záznam uzlu podle id, nebo NULL. */
const struct kivfs_node_addr *kivfs_config_find(const struct kivfs_config *c,
                                                uint32_t node_id);

/* Otisk seznamu [nodes] (64bitový FNV-1a přes seřazené trojice
 * id, host, port). Na všech uzlech se stejným seznamem vyjde stejně;
 * vlastní id, workers ani max_conns do něj nevstupují. Posílá se v HELLO
 * mezi uzly, aby se nepropojily uzly s různými konfiguracemi.
 * Host se porovnává doslova: "node1" a "NODE1" jsou různé seznamy. */
uint64_t kivfs_config_fingerprint(const struct kivfs_config *c);

#endif /* KIVFS_CONFIG_H */
