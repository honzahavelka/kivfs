#ifndef KIVFS_PEERS_H
#define KIVFS_PEERS_H

/*
 * KIVFS – tabulka peerů (spodní vrstva modulu Sync)                      
 * =================================================
 *
 * Jediné místo, které ví, který uzel je na kterém spojení. Má dvě poloviny,
 * které spolu sdílí jen jediný bit na peer ("nápověda živosti", viz níže):
 *
 *   A) ODCHOZÍ – já se ptám jiného uzlu (channel.h)
 *        jeden trvalý kanál na peer, půjčovaný pod mutexem peeru
 *        líné připojení + HELLO při půjčení, po neúspěchu backoff
 *        volají JEN klientští workery (a main jednou při startu)
 *
 *   B) PŘÍCHOZÍ – jiný uzel / klient se ptá mě (reactor.h)
 *        stav spojení conn_id -> NEZNÁMÉ / KLIENT / UZEL(node_id)
 *        HELLO projde uzlovou frontou jako každý jiný požadavek a vyřídí
 *        ho uzlový worker (kivfs_peers_in_hello); I/O vlákno tabulku jen
 *        čte (on_packet) a maže (on_close) -> tabulka je pod mutexem
 *        (in_lock, drží se jen na dobu čtení/zápisu položky)
 *
 * Mezi dvěma uzly jsou 2 TCP spojení: můj odchozí kanál k B je u B příchozí
 * spojení a naopak. Směry jsou nezávislé, takže při současném startu není
 * co deduplikovat.
 *
 * HELLO mezi uzly (rozšíření protocol.h, klient se nemění):
 *   požadavek: [u32 node_id]                    CLI klient (node_id = 0)
 *              [u32 node_id][u64 cluster_fp]    uzel
 *   odpověď:   [u64 ref][u32 node_id]
 *   cluster_fp = kivfs_config_fingerprint() – otisk seznamu [nodes].
 *
 *   Příjemce (B, uzlový worker):
 *     - node_id není v konfiguraci, je to moje vlastní id, nebo nesedí
 *       cluster_fp -> odpověď REJECTED a spojení se po odeslání zavře
 *     - node_id už má jiné živé příchozí spojení -> NOVĚJŠÍ VYHRÁVÁ:
 *       staré spojení se zavře (viz "Edge cases" níže)
 *     - jinak conn_id -> UZEL(node_id); další pakety jdou do uzlové fronty
 *   Žadatel (A, v kivfs_peers_acquire):
 *     - status != OK, nebo v odpovědi jiný node_id, než jaký má adresa
 *       v konfiguraci -> KIVFS_EPEERREJECT, kanál zavřít, backoff
 *
 * Nápověda živosti (B -> A): když mi uzel X úspěšně pošle HELLO, X právě
 * žije. Příchozí polovina proto nastaví u peeru X příznak alive_hint a
 * příští kivfs_peers_acquire(X) ignoruje případný backoff a zkusí se
 * připojit hned. Nic se tím nepřipojuje (uzlový worker nesmí čekat na
 * jiný uzel), jen
 * se neodmítne pokus kvůli backoffu, který zbyl z doby, kdy X neběžel.
 * Příznak je pod vlastním krátkým mutexem peeru (hint_lock), NE pod
 * mutexem kanálu: ten může půjčující vlákno držet až connect_timeout_ms
 * a uzlový worker na něj nesmí čekat. acquire příznak přečte a vynuluje.
 * Pod hint_lock jsou i stavové údaje pro kivfs_peers_snapshot (otevřeno /
 * poslední chyba), aby diagnostika nečekala na rozpracovaný connect.
 *
 * "HELLO první": na spojení ve stavu NEZNÁMÉ smí přijít jen HELLO;
 * cokoli jiného (i druhé HELLO na už představeném spojení) -> zavřít.
 *
 * Timestamp v hlavičce odchozích požadavků nastavuje link (čítač na peer,
 * přežije reconnect, takže je monotónní v rámci dvojice uzlů). Lamportovy
 * hodiny pro uspořádání operací sem NEPATŘÍ – pojedou v payloadu (Sync).
 *
 * Návratové kódy: errors.h.
 */

#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "packet.h"
#include "reactor.h"     /* jen kivfs_conn_id */

struct kivfs_peers;       /* neprůhledné, zná jen peers.c */

struct kivfs_peers_config {
    const struct kivfs_config *cfg;   /* musí žít déle než peers */
    int connect_timeout_ms;           /* connect + HELLO, např. 1000 */
    int io_timeout_ms;                /* každé čtení/zápis potom, např. 5000 */
    int backoff_ms;                   /* po neúspěšném připojení, např. 1500 */
};

/* Vytvoří tabulku: odchozí kanály zavřené, příchozí tabulka prázdná
 * (velikost cfg->max_conns – spoléhá na záruku z reactor.h, že slot
 * v conn_id < max_conns). Nic nepřipojuje.
 * KIVFS_OK, KIVFS_EINVAL (nesmyslná konfigurace), KIVFS_ENOMEM. */
int  kivfs_peers_create(struct kivfs_peers **out, const struct kivfs_peers_config *pc);

/* Zavře odchozí kanály a uvolní vše. Volat až nikdo nedrží link a reactor
 * neběží (pořadí: stop reactor -> join io -> stop workery -> destroy). */
void kivfs_peers_destroy(struct kivfs_peers *p);

/* ================================================================== */
/* A) ODCHOZÍ                                                         */
/* ================================================================== */

/* Půjčený kanál k jednomu uzlu. Platný od acquire do release, používá ho
 * jen vlákno, které ho půjčilo. Po JAKÉKOLI chybě send/recv/call se link
 * sám označí za mrtvý a release kanál zavře (pravidlo channel.h). */
struct kivfs_peer_link;

/* Při startu (main, PO spuštění reactoru a workerů, jinak by se dva
 * současně startující uzly navzájem neodbavily): zkusí se jednou připojit
 * ke všem peerům a výsledek zaloguje. Chyby ignoruje, nikdy nečeká déle
 * než (n_nodes - 1) * connect_timeout_ms. */
void kivfs_peers_connect_all(struct kivfs_peers *p);

/* [TS] Půjčí (zamkne) kanál k node_id. Je-li kanál zavřený nebo zastaralý
 * (protistrana ho mezitím zavřela – pozná se bez odeslání, viz
 * kivfs_chan_is_stale), připojí se a provede HELLO; během backoffu to
 * ani nezkouší a hned vrátí chybu.
 *   KIVFS_OK          *out platný, nutno vrátit přes release
 *   KIVFS_EPEERDOWN   nelze se připojit / běží backoff a peer od té doby
 *                     neposlal HELLO (nic se nedrží)
 *   KIVFS_EPEERREJECT HELLO neprošlo (nic se nedrží)
 *   KIVFS_EINVAL      neznámé node_id nebo moje vlastní */
int  kivfs_peers_acquire(struct kivfs_peers *p, uint32_t node_id,
                         struct kivfs_peer_link **out);

/* [TS] Půjčí kanály ke VŠEM ostatním uzlům, zamyká vzestupně podle
 * node_id (proti deadlocku). Všechno, nebo nic: selže-li jediný, vrátí
 * už půjčené a vrátí jeho chybu; *failed_node = kdo selhal (pro log).
 * out musí mít místo pro cfg->n_nodes - 1 položek; *n = jejich počet.
 * Pro kolo PROPOSE v kroku 2; selhání -> klient dostane BUSY. */
int  kivfs_peers_acquire_all(struct kivfs_peers *p, struct kivfs_peer_link **out,
                             size_t *n, uint32_t *failed_node);

/* [TS] Vrátí kanál (mrtvý zavře a nastaví backoff). NULL je no-op. */
void kivfs_peers_release(struct kivfs_peers *p, struct kivfs_peer_link *l);
void kivfs_peers_release_all(struct kivfs_peers *p, struct kivfs_peer_link **l, size_t n);

uint32_t kivfs_link_node_id(const struct kivfs_peer_link *l);

/* Odeslání / příjem na půjčeném kanálu. send nastaví req->header.timestamp
 * (čítač linku), jinak paket nemění. recv přijme odpověď a ověří, že patří
 * k req (stejně jako kivfs_chan_call). call = send + recv.
 * Send-all / recv-all: send na všechny linky, pak recv ze všech. */
int  kivfs_link_send(struct kivfs_peer_link *l, struct kivfs_packet *req);
int  kivfs_link_recv(struct kivfs_peer_link *l, const struct kivfs_packet *req,
                     struct kivfs_packet *resp);
int  kivfs_link_call(struct kivfs_peer_link *l, struct kivfs_packet *req,
                     struct kivfs_packet *resp);

/* ================================================================== */
/* B) PŘÍCHOZÍ                                                       */
/*    kind, closed: I/O vlákno (on_packet, on_close)                  */
/*    in_hello:     uzlový worker                                     */
/* ================================================================== */

enum kivfs_conn_kind {
    KIVFS_CONN_UNKNOWN = 0,   /* ještě neproběhlo HELLO */
    KIVFS_CONN_CLIENT,        /* HELLO s node_id 0      -> klientská fronta */
    KIVFS_CONN_NODE           /* HELLO s platným uzlem  -> uzlová fronta */
};

/* Kdo je na spojení; u KIVFS_CONN_NODE vyplní *node_id (smí být NULL). */
enum kivfs_conn_kind kivfs_peers_in_kind(struct kivfs_peers *p,
                                         kivfs_conn_id id, uint32_t *node_id);

/* Výsledek HELLO pro volajícího (uzlového workera), který pak odešle
 * odpověď přes kivfs_reactor_send a případně zavře spojení.
 * Pořadí je důležité: tabulka je zapsaná UVNITŘ in_hello, tedy PŘED
 * odesláním odpovědi. Reactor další paket ze spojení přečte až po
 * odeslání odpovědi, takže on_packet už vidí UZEL(x) a pošle ho do
 * správné fronty. */
struct kivfs_hello_result {
    int           close_after;    /* po odeslání odpovědi spojení zavřít */
    kivfs_conn_id replaced;       /* staré spojení téhož uzlu k zavření,
                                   * nebo KIVFS_CONN_ID_INVALID */
    char          note[96];       /* pro log */
};

/* Vyřídí HELLO na spojení id: validace, zápis do tabulky, nápověda živosti
 * pro odchozí kanál k témuž uzlu (jen při úspěchu), sestaví odpověď
 * do *resp s hlavičkovým timestampem resp_ts. Peers nesahá na reactor –
 * odeslání a zavření udělá volající podle *res.
 *   KIVFS_OK      *resp je hotová odpověď (i chybová: MALFORMED, REJECTED)
 *   KIVFS_ENOMEM  *resp prázdný -> volající spojení zavře */
int  kivfs_peers_in_hello(struct kivfs_peers *p, kivfs_conn_id id,
                          const struct kivfs_packet *req, uint64_t resp_ts,
                          struct kivfs_packet *resp, struct kivfs_hello_result *res);

/* on_close: smaže spojení z tabulky (neznámé id ignoruje). */
void kivfs_peers_in_closed(struct kivfs_peers *p, kivfs_conn_id id);

/* ================================================================== */
/* Diagnostika                                                        */
/* ================================================================== */

struct kivfs_peer_status {
    uint32_t node_id;
    int      out_connected;   /* odchozí kanál otevřený (stav v tuto chvíli) */
    int      out_last_err;    /* poslední chyba připojení/HELLO, 0 = žádná */
    int      in_connected;    /* má živé příchozí spojení */
};

/* [TS] Stav všech ostatních uzlů (pro log a příkaz klienta "nodes").
 * Příchozí část se čte pod in_lock. Vrací počet položek. */
size_t kivfs_peers_snapshot(struct kivfs_peers *p, struct kivfs_peer_status *out,
                            size_t max);

#endif /* KIVFS_PEERS_H */
