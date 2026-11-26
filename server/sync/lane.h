#ifndef KIVFS_SYNC_LANE_H
#define KIVFS_SYNC_LANE_H

/*
 * KIVFS – pruh Syncu: Lamportovy hodiny + seřazená fronta operací
 * (25. 11.; implementace lane.c, test test_lane)
 * ===============================================================
 *
 * Jeden pruh = jedny hodiny (Tcli nebo Tdb) a jedna fronta. Sync má dva
 * nezávislé pruhy (KIVFS_LANE_CLI, KIVFS_LANE_DB). Pravidla a zprávy:
 * syncproto.h, rozhodnutí: claude/architektura.md (25. 11.).
 *
 * Čistá datová struktura BEZ SÍTĚ a bez času: nic neposílá, nic nečeká,
 * neví o pohledech ani epochách (kontroluje volající Sync). Díky tomu je
 * testovatelná sama o sobě (i na macOS, jako backend_mock). Volá ji jen
 * worker Syncu – žádné zámky.
 *
 * Fronta = jedna posloupnost seřazená podle klíče (t, op_id), viz
 * kivfs_sync_key_cmp. Originální dvojice front se na ni mapuje stavem:
 *   async-*  = položky PROPOSED  (čekají na dohodu, t = návrh)
 *   sync-*   = položky COMMITTED (dohodnuté, t = T_final)
 * Klíč položky je vždy její AKTUÁLNÍ t: při COMMIT se t změní na T_final
 * (>= návrh) a položka se v pořadí posune dozadu.
 *
 * STABILITA (Skeen): hlava fronty smí k provedení, jen když je COMMITTED.
 * Platí to proto, že finální t každé čekající položky je >= jejímu
 * návrhu, takže nic, co je ve frontě za hlavou, už před ni nepředběhne.
 * Podmínka: příjemce musí operaci zařadit UŽ PŘI PROPOSE (pruh DB)
 * a každý jeho další návrh je větší než všechny předchozí (T roste).
 *
 * Co se do fronty dostane:
 *   pruh DB:  vlastní operace (begin) i cizí (on_propose) – provádějí se
 *             na všech uzlech
 *   pruh CLI: JEN vlastní operace (begin). Cizí se neřadí – on_propose jen
 *             posune hodiny (klientské operace se provádějí jen u původce).
 *
 * Provádění je SÉRIOVÉ: najednou je nejvýš jedna položka EXECUTING.
 *
 * HOLD (bod 13 a, 25. 11.): položka s hold > 0 se k provedení nevydá,
 * i když je COMMITTED a v čele – a stojí za ní i vše ostatní (pořadí je
 * celé dané klíčem). Sync tak u původce drží vlastní DB operaci, dokud
 * COMMIT nepotvrdí všechny uzly (kivfs_lane_hold / kivfs_lane_release).
 *
 * Index (jen pruh DB): každá provedená operace dostane při kivfs_lane_next
 * pořadové číslo last_index + 1. Pořadí provedení je na všech uzlech
 * stejné, takže i indexy jsou stejné – podle nich se archivuje
 * (MESSAGE_LOGS) a obnovuje. Index se spotřebuje i operací, jejíž
 * podmínka v DB neprojde (EXISTS…): i ta je součástí posloupnosti.
 *
 * Návratové kódy: errors.h, mimo jiné:
 *   KIVFS_ENOENT = -43   operace s tímto op_id ve frontě není
 *   KIVFS_EPROTO = -44   porušení protokolu protistranou (COMMIT s t
 *                        menším než návrh, ABORT potvrzené operace…)
 */

#include <stddef.h>
#include <stdint.h>
#include "proto/syncproto.h"

struct kivfs_lane;

enum kivfs_lane_state {
    KIVFS_LANE_PROPOSED  = 1,    /* = KIVFS_OPST_PROPOSED */
    KIVFS_LANE_COMMITTED = 2,    /* = KIVFS_OPST_COMMITTED */
    KIVFS_LANE_EXECUTING = 3     /* vydána přes kivfs_lane_next, čeká na finish */
};

/* Položka fronty. Ukazatel je platný od vzniku do kivfs_lane_finish /
 * kivfs_lane_abort / kivfs_lane_drop_origin / kivfs_lane_destroy.
 * Pole jsou pro volajícího JEN KE ČTENÍ, kromě user. */
struct kivfs_lane_entry {
    struct kivfs_op_id op;
    uint64_t           t;          /* aktuální klíč: návrh, po COMMIT T_final */
    uint64_t           t_proposed; /* můj návrh (u původce Ttmp) – pro QUERY */
    uint32_t           state;      /* enum kivfs_lane_state */
    int                mine;       /* 1 = operace tohoto uzlu (begin) */
    uint32_t           hold;       /* > 0: k provedení se nevydá (viz HOLD);
                                    * mění jen kivfs_lane_hold / _release */
    uint64_t           index;      /* pruh DB: přidělen při next, jinak 0 */
    const uint8_t     *payload;    /* pruh DB: DB operace ([dbop] + záznamy,
                                    * vlastní kopie pruhu); jinak NULL */
    uint32_t           payload_len;
    void              *user;       /* volajícího (u původce kontext požadavku,
                                    * který čeká na výsledek); pruh ho neuvolňuje */
};

struct kivfs_lane_limits {
    uint32_t max_entries;          /* položek ve frontě celkem (vlastní i cizí) */
    uint64_t max_bytes;            /* součet payload_len (pruh DB) */
};

/* ------------------------------------------------------------------ */
/* Život                                                               */
/* ------------------------------------------------------------------ */

/* lane = KIVFS_LANE_CLI / _DB; self = moje node_id a incarnation (z HELLO),
 * z nich a z čítače seq vzniká op_id vlastních operací. Hodiny začínají
 * na 0, last_index na 0.
 * KIVFS_OK, KIVFS_ENOMEM, KIVFS_EINVAL (lane, nulové limity). */
int  kivfs_lane_create(struct kivfs_lane **out, uint32_t lane, uint32_t self_node,
                       uint64_t self_incarnation, const struct kivfs_lane_limits *lim);

/* Uvolní frontu i kopie payloadů; user položek NEuvolňuje – volající si
 * je předtím projde (kivfs_lane_foreach) a odpoví na čekající požadavky. */
void kivfs_lane_destroy(struct kivfs_lane *l);

/* ------------------------------------------------------------------ */
/* Hodiny a obnova                                                     */
/* ------------------------------------------------------------------ */

uint64_t kivfs_lane_clock(const struct kivfs_lane *l);
uint64_t kivfs_lane_last_index(const struct kivfs_lane *l);

/* last_index z DB (DB_LOG_HEAD) – smí se nastavit, kdykoli se nic
 * neprovádí (žádná položka EXECUTING); fronta smí obsahovat PROPOSED
 * i COMMITTED (po startu Syncu sem cizí PROPOSE dorazí dřív než odpověď
 * DB). Jinak KIVFS_EINVAL. Hodiny se při startu NEobnovují – posunou se
 * samy první zprávou (7.4.3); kvůli stabilitě to nevadí, protože každá
 * přijatá zpráva hodiny posune nad svoje t. */
int  kivfs_lane_set_last_index(struct kivfs_lane *l, uint64_t last_index);

/* ------------------------------------------------------------------ */
/* Kolo hlasování                                                      */
/* ------------------------------------------------------------------ */

/* PŮVODCE: nová vlastní operace. T = T + 1 (= Ttmp), přidělí op_id
 * (seq + 1), zařadí jako PROPOSED s t = Ttmp, payload si zkopíruje
 * (pruh DB; u pruhu CLI musí být NULL/0). Volající pak rozešle
 * SYNC_PROPOSE s (*out)->op a (*out)->t.
 *   KIVFS_OK; KIVFS_EAGAIN (limity -> odpověz BUSY); KIVFS_ENOMEM;
 *   KIVFS_EINVAL (payload u CLI / chybí u DB). */
int  kivfs_lane_begin(struct kivfs_lane *l, const uint8_t *payload, uint32_t payload_len,
                      void *user, struct kivfs_lane_entry **out);

/* PŘÍJEMCE: SYNC_PROPOSE od jiného uzlu.
 *   *t_out = max(t_in, T + 1), T = *t_out (pravidlo 7.4.1, d = 1)
 *   pruh DB:  zařadí PROPOSED s t = *t_out a kopií payloadu
 *   pruh CLI: nic neřadí (payload musí být NULL/0)
 * Duplicita (op_id už ve frontě, např. opakování po reconnectu): nic
 * nemění, *t_out = dřívější návrh (t_proposed), KIVFS_OK – odpověď je
 * stejná jako poprvé. Duplicitu operace, která už byla PROVEDENA, pruh
 * nepozná (z fronty zmizela) – hlídá volající podle indexu/archivu.
 *   KIVFS_OK; KIVFS_EAGAIN (limity -> BUSY, hodiny se NEposunou);
 *   KIVFS_ENOMEM; KIVFS_EINVAL (op.node = já, payload u CLI / chybí u DB). */
int  kivfs_lane_on_propose(struct kivfs_lane *l, const struct kivfs_op_id *op, uint64_t t_in,
                           const uint8_t *payload, uint32_t payload_len, uint64_t *t_out);

/* PŮVODCE i PŘÍJEMCE: dohodnuté T_final (u původce = max z odpovědí
 * a vlastního návrhu; volající ho pak rozešle SYNC_COMMIT).
 * VŽDY nejdřív T = max(T, t_final) – i když operaci nezná (bez toho by
 * nová operace mohla dostat menší t než už provedená).
 * Pak, je-li op ve frontě: PROPOSED -> COMMITTED, t = t_final, přeřazení.
 *   KIVFS_OK
 *   KIVFS_ENOENT  op ve frontě není. Pruh CLI u příjemce: normální stav
 *                 (nic se neřadí) – ignorovat. Pruh DB: PROPOSE se ztratil
 *                 nebo už byla provedena -> volající rozhodne (obnova).
 *   KIVFS_EPROTO  t_final < t_proposed, nebo op už je COMMITTED s jiným
 *                 t_final (opakovaný COMMIT se STEJNÝM t = KIVFS_OK). */
int  kivfs_lane_commit(struct kivfs_lane *l, const struct kivfs_op_id *op, uint64_t t_final);

/* PŮVODCE (kolo selhalo – timeout, uzel neodpověděl) i PŘÍJEMCE
 * (SYNC_ABORT): odstraní PROPOSED položku. Vrátí její user (u původce
 * kontext, kterému volající odpoví chybou), jinak NULL.
 *   KIVFS_OK; KIVFS_ENOENT (neznámá – u příjemce běžné, ignorovat);
 *   KIVFS_EPROTO (COMMITTED/EXECUTING – potvrzenou operaci nelze zrušit). */
int  kivfs_lane_abort(struct kivfs_lane *l, const struct kivfs_op_id *op, void **user_out);

/* ------------------------------------------------------------------ */
/* Provádění                                                           */
/* ------------------------------------------------------------------ */

/* Pozdrží provedení operace: hold = n (n = 0 zruší). Platí ve stavu
 * PROPOSED i COMMITTED; kivfs_lane_next hlavu s hold > 0 nevydá.
 *   KIVFS_OK; KIVFS_ENOENT; KIVFS_EPROTO (už EXECUTING). */
int  kivfs_lane_hold(struct kivfs_lane *l, const struct kivfs_op_id *op, uint32_t n);

/* hold - 1 (při 0 nic). KIVFS_OK / KIVFS_ENOENT. Po release volat
 * kivfs_lane_next – hlava se mohla uvolnit. */
int  kivfs_lane_release(struct kivfs_lane *l, const struct kivfs_op_id *op);

/* Další operace k provedení: hlava fronty, pokud je COMMITTED, nemá hold
 * a žádná jiná není EXECUTING. Označí ji EXECUTING, u pruhu DB přidělí
 * index = last_index + 1 (last_index se posune až ve finish).
 * NULL = teď nic (prázdno, hlava čeká na dohodu / na hold, nebo se už
 * provádí).
 * Volat po každém commit/abort/finish – kterékoli z nich může hlavu
 * uvolnit. */
struct kivfs_lane_entry *kivfs_lane_next(struct kivfs_lane *l);

/* Operace vydaná přes next je provedená (u pruhu DB: DB_APPLY vrátil
 * odpověď – i neúspěšnou podmínku jako EXISTS). Odstraní ji z fronty,
 * pruh DB: last_index = e->index. Vrátí e->user (u původce kontext
 * k odpovědi). Selhání DB_APPLY s INTERNAL / bez odpovědi finish NEvolá:
 * uzel nesmí pokračovat dál (díra v posloupnosti) – řeší Sync (vypadne
 * z pohledu, obnova). */
void *kivfs_lane_finish(struct kivfs_lane *l, struct kivfs_lane_entry *e);

/* ------------------------------------------------------------------ */
/* Dotazy a úklid                                                      */
/* ------------------------------------------------------------------ */

/* Položka podle op_id, nebo NULL (pro SYNC_QUERY, duplicity). */
struct kivfs_lane_entry *kivfs_lane_find(struct kivfs_lane *l, const struct kivfs_op_id *op);

size_t kivfs_lane_count(const struct kivfs_lane *l);

/* Projde položky v pořadí fronty; cb vrátí nenulu = přestat.
 * cb NESMÍ frontu měnit. */
void kivfs_lane_foreach(struct kivfs_lane *l,
                        int (*cb)(struct kivfs_lane_entry *e, void *arg), void *arg);

/* Uzel node se restartoval s novou incarnation (HELLO) – jeho starý běh
 * už COMMIT nepošle. Odstraní PROPOSED položky od (node, incarnation !=
 * current_incarnation); COMMITTED ponechá (provedou se). Vrátí počet.
 * POZOR: bezpečné jen tehdy, když COMMIT té operace nedostal NIKDO –
 * jinak se repliky rozejdou. Volající se proto nejdřív zeptá ostatních
 * (SYNC_QUERY); v kroku „pevný pohled“ stačí: pokud někdo COMMIT má,
 * přepošle ho a commit proběhne místo zahození. */
size_t kivfs_lane_drop_origin(struct kivfs_lane *l, uint32_t node, uint64_t current_incarnation);

#endif /* KIVFS_SYNC_LANE_H */
