#ifndef KIVFS_SYNC_H
#define KIVFS_SYNC_H

/*
 * KIVFS – modul Sync: globální pořadí operací             *** NÁVRH ***
 * =============================================
 *
 * Podle kap. 7.4.1 původní práce. Krok 2 implementuje jen hodiny Tcli
 * (požadavky klientů); Tdb (replikace DB) přijde v kroku 4 se stejnými
 * zprávami, jen s jiným druhem hodin.
 *
 * Fronty a vlákna (originál: dvojice front na každé hodiny, každá fronta
 * má vlastní vlákno):
 *
 *   klientský worker                         (Auth, pak kivfs_sync_run_cli)
 *        |  vloží žádost, čeká
 *        v
 *   async-client  ->  [vlákno async-cli]  kolo PROPOSE/COMMIT, přidělí T
 *        |                                (jedno kolo po druhém, FIFO)
 *        v
 *   sync-client   ->  [vlákno sync-cli]   zavolá op->exec (VFS -> DB)
 *        |                                sériově v pořadí T
 *        v
 *   klientský worker se probudí a odpoví klientovi
 *
 *   Protože async-cli dělá kola jedno po druhém, značky lokálních operací
 *   rostou v pořadí vložení a sync-client stačí obyčejné FIFO.
 *
 * Kolo (vlákno async-cli), d = 1:
 *   1. pod state_lock: Ttmp = Tcli + 1; Tcli = Ttmp
 *   2. kivfs_peers_acquire_all; selže -> op->rc = KIVFS_EPEERDOWN (klient BUSY)
 *   3. send-all SYNC_PROPOSE(Ttmp), recv-all Tout_i
 *      jakákoli chyba -> EPEERDOWN; NENÍ co vracet zpět (u Tcli si příjemci
 *      nic neukládají, posunuté hodiny jen rostou)
 *   4. T = max(Ttmp, Tout_i); pod state_lock: Tcli = max(Tcli, T)
 *   5. send-all SYNC_COMMIT(T), recv-all; chyby se JEN logují: každé
 *      pozdější kolo kohokoli se ptá všech uzlů včetně mě (mám Tcli >= T),
 *      takže dostane větší T i bez COMMIT. U Tcli je COMMIT optimalizace.
 *   6. release_all, op do sync-client
 *   Vlákno NIKDY nedrží state_lock během síťové komunikace – jinak by
 *   uzlový worker druhého uzlu při souběžném kole čekal na mutex a uzly
 *   by se zablokovaly navzájem.
 *
 * Příjemce (uzlový worker, kivfs_sync_on_propose / on_commit):
 *   PROPOSE: pod state_lock Tout = max(Tin, T + 1); T = Tout  (rozdíl proti
 *            originálu: hodiny se posunou už tady – standardní Lamport,
 *            méně shod, v kroku 4 potřeba pro čekající záznamy)
 *   COMMIT:  pod state_lock T = max(T, Tc)
 *   Jen krátký mutex, žádná síť, žádné čekání na vlákna Sync.
 *
 * Úplné pořadí: (T, node_id žadatele). Dvě souběžná kola ze dvou uzlů
 * mohou skončit se stejným T; rozhodne node_id.
 *
 * Restart uzlu: hodiny začínají od 0, ale už první kolo vezme maximum
 * přes všechny uzly a dorovná je.
 *
 * Zprávy (protocol.h, nové kódy):
 *   SYNC_PROPOSE = 4  [u32 hodiny][u64 op_id][u64 Ttmp]  -> [u64 ref][u64 Tout]
 *   SYNC_COMMIT  = 5  [u32 hodiny][u64 op_id][u64 T]     -> [u64 ref]
 *     hodiny = enum kivfs_sync_clock; op_id = čítač žadatele. Odesílatel je
 *     známý ze spojení, takže (odesílatel, op_id) jednoznačně určí operaci –
 *     klíč pro čekající záznamy a budoucí ABORT v kroku 4. V kroku 4 se do
 *     PROPOSE přidá SQL. Hodiny DB zatím -> UNKNOWN_REQ.
 *   SYNC_TEST    = 6  (klient) nic -> [u64 ref][u64 T][u32 node_id]
 *     testovací operace: projde celým kolem, exec jen zaznamená T.
 *
 * Návratové kódy: errors.h.
 */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "packet.h"
#include "peers.h"

enum kivfs_sync_clock {
    KIVFS_SYNC_CLOCK_CLI = 1,   /* Tcli – požadavky klientů (krok 2) */
    KIVFS_SYNC_CLOCK_DB  = 2    /* Tdb  – replikace DB (krok 4)      */
};

/* Globální pořadí operace. Porovnání: nejdřív t, při shodě node_id. */
struct kivfs_sync_ts {
    uint64_t t;
    uint32_t node_id;           /* žadatel */
};

/* <0, 0, >0 jako strcmp. */
int kivfs_sync_ts_cmp(struct kivfs_sync_ts a, struct kivfs_sync_ts b);

struct kivfs_sync;              /* neprůhledné, zná jen sync.c */

/* ------------------------------------------------------------------ */
/* Operace k seřazení a provedení                                      */
/* ------------------------------------------------------------------ */

/* Vlastní ji volající (typicky na zásobníku klientského workera) a musí
 * žít, dokud kivfs_sync_run_cli nevrátí. Sync ji jen půjčuje svým vláknům. */
struct kivfs_sync_op {
    /* --- vstup --- */
    /* Provedení operace; volá ho vlákno sync-cli v pořadí podle ts.
     * Smí volat VFS/DB (blokovat na disku či DB), NESMÍ čekat na jiné
     * uzly ani na vlákna Sync. Výsledek si uloží přes arg. */
    void  (*exec)(struct kivfs_sync_op *op);
    void   *arg;

    /* --- výstup (platné po návratu kivfs_sync_run_cli) --- */
    int                  rc;    /* KIVFS_OK: seřazeno a provedeno
                                 * KIVFS_EPEERDOWN: cluster neúplný -> BUSY,
                                 *   exec se NEVOLAL
                                 * KIVFS_ESHUTDOWN: uzel se vypíná, exec se
                                 *   nevolal */
    struct kivfs_sync_ts ts;    /* přidělené pořadí (při rc == KIVFS_OK) */

    /* --- interní (nesahat) --- */
    pthread_mutex_t      done_lock;
    pthread_cond_t       done_cond;
    int                  done;
};

/* [TS] Klientský worker: seřadí operaci kolem PROPOSE/COMMIT a nechá ji
 * provést vláknem sync-cli. BLOKUJE, dokud není op->exec hotový, nebo
 * dokud se nezjistí, že provést nejde (op->rc). Smí volat jen vlákno,
 * které smí čekat na jiné uzly (klientský worker, NE uzlový).
 * Návratová hodnota = op->rc. */
int  kivfs_sync_run_cli(struct kivfs_sync *s, struct kivfs_sync_op *op);

/* ------------------------------------------------------------------ */
/* Příjem zpráv od jiných uzlů – uzlový worker                         */
/* ------------------------------------------------------------------ */

/* [TS] SYNC_PROPOSE / SYNC_COMMIT od uzlu from_node. Sestaví odpověď do
 * *resp (i chybovou: MALFORMED, UNKNOWN_REQ pro hodiny DB) s hlavičkovým
 * timestampem resp_ts. Jen krátký mutex, nikdy nečeká na síť.
 *   KIVFS_OK nebo KIVFS_ENOMEM (resp prázdný -> volající zavře spojení) */
int  kivfs_sync_on_propose(struct kivfs_sync *s, uint32_t from_node,
                           const struct kivfs_packet *req, uint64_t resp_ts,
                           struct kivfs_packet *resp);
int  kivfs_sync_on_commit(struct kivfs_sync *s, uint32_t from_node,
                          const struct kivfs_packet *req, uint64_t resp_ts,
                          struct kivfs_packet *resp);

/* ------------------------------------------------------------------ */
/* Životní cyklus                                                      */
/* ------------------------------------------------------------------ */

struct kivfs_sync_config {
    const struct kivfs_config *cfg;    /* self_id; musí žít déle než sync */
    struct kivfs_peers        *peers;  /* kanály k ostatním uzlům */
    size_t                     max_ops;/* kapacita front = počet klientských
                                        * workerů (každý má nejvýš 1 operaci) */
};

/* Vytvoří stav (hodiny 0, prázdné fronty). Vlákna ještě neběží. */
int  kivfs_sync_create(struct kivfs_sync **out, const struct kivfs_sync_config *sc);

/* Spustí vlákna async-cli a sync-cli. Volat po spuštění reactoru (kola
 * potřebují, aby nám ostatní uzly mohly odpovídat... a my jim). */
int  kivfs_sync_start(struct kivfs_sync *s);

/* Zastaví vlákna a počká na ně. Operace ještě neseřazené (async-client)
 * dostanou KIVFS_ESHUTDOWN; už seřazené (sync-client) se ještě provedou –
 * ostatní uzly o jejich T vědí. Rozběhnuté kolo doběhne (max. I/O timeout
 * peers). Pořadí v main: stop reactor -> join io -> kivfs_sync_stop ->
 * stop workery -> destroy (klientský worker čekající v run_cli se tak
 * vždy probudí). */
void kivfs_sync_stop(struct kivfs_sync *s);

void kivfs_sync_destroy(struct kivfs_sync *s);

/* [TS] Aktuální hodnota hodin (diagnostika, testy). */
uint64_t kivfs_sync_clock(struct kivfs_sync *s, enum kivfs_sync_clock c);

#endif /* KIVFS_SYNC_H */
