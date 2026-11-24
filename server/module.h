#ifndef KIVFS_MODULE_H
#define KIVFS_MODULE_H

/*
 * KIVFS – společná kostra serverového modulu
 * ==========================================
 *
 * Každý modul (kivfs-auth, -sync, -vfs, -db, -fs) je samostatný program,
 * jehož main jen vyplní kivfs_mod_ops a zavolá kivfs_module_main. Kostra
 * udělá všechno společné:
 *
 *   - argumenty: -c <soubor.ini>
 *   - konfigurace (modconf.h), fail fast: chyba -> exit 1
 *   - reactor + fronta událostí (events.h), jeden worker, vlákno pro
 *     signály (SIGINT/SIGTERM -> řádné ukončení; handler signálu sám
 *     nesmí volat kivfs_reactor_stop, bere mutex)
 *   - LINKY = odchozí spojení na moduly, které tento modul volá (Auth ->
 *     Sync -> VFS -> DB, FS; Sync navíc na Synce všech ostatních uzlů):
 *     connect -> HELLO -> UP; při pádu DOWN a nový pokus s exponenciálním
 *     backoffem 100 ms .. 5 s (časovač, worker nikdy nespí)
 *   - příchozí spojení: HELLO (kontrola, kdo se smí připojit – hello.h),
 *     PING, STATUS (kaskáda, protocol.h); ostatní požadavky -> on_request
 *
 * Stav modulu vlastní JEDINÝ worker – žádné zámky. Vlákna:
 *   hlavní = I/O (reactor), worker = veškerá logika, signály = jen stop.
 *
 * Log na stderr: "čas [nodeN modul] zpráva".
 */

#include "events.h"
#include "modconf.h"
#include "packet.h"

struct kivfs_mod;

/* Představená protistrana příchozího spojení. */
struct kivfs_mod_peer {
    kivfs_conn_id conn;
    uint32_t      module;       /* enum kivfs_module */
    uint32_t      node;
    uint64_t      incarnation;
};

struct kivfs_mod_ops {
    enum kivfs_module  self;
    const char *const *extra_keys;   /* klíče navíc v sekci modulu, NULL-terminated */

    /* Po načtení konfigurace, před spuštěním sítě (např. otevřít backend
     * DB, přečíst extra_keys). Nenulový návrat = chyba startu, exit 1.
     * Stav modulu si ulož přes kivfs_mod_set_user. NULL = nic. */
    int  (*init)(struct kivfs_mod *m, const struct kivfs_config *cfg);

    /* Po zastavení workeru, před uvolněním kostry. NULL = nic. */
    void (*fini)(struct kivfs_mod *m);

    /* Požadavek s vlastním request_code od protistrany, která prošla HELLO.
     * Volá se ve workeru. Vrací 1 = převzato (modul odpoví sám – hned nebo
     * později přes kivfs_mod_reply; req si smí vzít, pak ho vynuluje),
     * 0 = neznám -> kostra odpoví UNKNOWN_REQ. NULL = žádné. */
    int  (*on_request)(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                       struct kivfs_packet *req);
};

/* ------------------------------------------------------------------ */
/* Pomocné funkce pro modul – volat JEN z workeru (on_request, cont)   */
/* ------------------------------------------------------------------ */

void    *kivfs_mod_user(const struct kivfs_mod *m);
void     kivfs_mod_set_user(struct kivfs_mod *m, void *user);
const struct kivfs_modconf *kivfs_mod_conf(const struct kivfs_mod *m);

/* Timestamp pro další odesílaný paket (čítač modulu). */
uint64_t kivfs_mod_next_ts(struct kivfs_mod *m);

/* Odpověď na příchozí požadavek (převezme resp vždy; mrtvé spojení ->
 * tiše zahozeno). */
void     kivfs_mod_reply(struct kivfs_mod *m, kivfs_conn_id to, struct kivfs_packet *resp);

/* Odpověď jen se stavem (bez dat). */
void     kivfs_mod_reply_status(struct kivfs_mod *m, kivfs_conn_id to,
                                const struct kivfs_packet *req, int16_t status);

/* Pokračování po kivfs_mod_call – voláno PRÁVĚ JEDNOU, ve workeru.
 *   err == KIVFS_OK: resp je odpověď (stav v resp->header.return_value);
 *                    cont ji smí převzít (pak ji vynuluje), jinak ji
 *                    kostra po návratu uvolní
 *   jinak resp == NULL: KIVFS_ETIMEOUT, KIVFS_ECONNLOST (výsledek
 *                    NEZNÁMÝ), KIVFS_ESHUTDOWN
 * state = ukazatel předaný do kivfs_mod_call; jeho paměť vlastní modul
 * (typicky ji v cont uvolní). */
typedef void (*kivfs_mod_cont)(struct kivfs_mod *m, void *state, int err,
                               struct kivfs_packet *resp);

/* Pošle požadavek modulu "to" na STEJNÉM uzlu (musí to být moje
 * závislost, např. VFS -> DB) a vrátí se hned; výsledek dostane cont.
 * Paket převezme vždy; timestamp nastaví sama.
 * Vrací KIVFS_OK (cont přijde právě jednou), jinak se cont NEvolá:
 *   KIVFS_ENOTREADY  linka na "to" není UP -> odpověz UNAVAILABLE
 *   KIVFS_EAGAIN     moc rozpracovaných volání -> odpověz BUSY
 *   KIVFS_EINVAL     "to" není moje závislost */
int      kivfs_mod_call(struct kivfs_mod *m, enum kivfs_module to, struct kivfs_packet *req,
                        int timeout_ms, kivfs_mod_cont cont, void *state);

/* Přepošle požadavek modulu "to" a jeho odpověď vrátí původnímu odesílateli
 * (záznamy za ref se zkopírují, stav zůstane). Pro moduly, které jen
 * předávají dál (Auth po vložení ctx, Sync zatím celý).
 *   from, orig  = kdo se ptal a jeho původní požadavek (kvůli ref)
 *   fwd         = paket, který se pošle dál (převezme se vždy); kód
 *                 požadavku se zachová
 * Chyby se odesílateli odpoví samy: linka dole -> UNAVAILABLE, plno ->
 * BUSY, timeout/pád spojení -> UNAVAILABLE. Vrací vždy "převzato". */
void     kivfs_mod_forward(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                           const struct kivfs_packet *orig, struct kivfs_packet *fwd,
                           enum kivfs_module to, int timeout_ms);

/* Celý život modulu; vrací exit kód procesu (0 = řádně ukončen signálem,
 * 1 = chyba konfigurace nebo startu). */
int kivfs_module_main(int argc, char **argv, const struct kivfs_mod_ops *ops);

#endif /* KIVFS_MODULE_H */
