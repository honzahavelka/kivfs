#ifndef KIVFS_SYNCPROTO_H
#define KIVFS_SYNCPROTO_H

/*
 * KIVFS – zprávy synchronizace na drátě (návrh 25. 11.)
 * ======================================================
 *
 * Hotové jsou struktury a jejich (de)serializace (syncproto.c, test
 * test_syncproto). Kódy a stavy jsou v protocol.h; Sync ani DB je zatím
 * neobsluhují.
 *
 * Podklad: původní KIVFS kap. 7.4.1 (hlasování o časové značce), 7.4.3
 * (obnova), 7.6 (MESSAGE_LOGS); rozhodnutí v claude/architektura.md,
 * sekce „Návrh Sync a replikace – rozhodnutí 25. 11.“.
 *
 * Dva nezávislé PRUHY se svými Lamportovými hodinami (d = 1):
 *   CLI  Tcli – klientské operace; provádějí se JEN u původce. Ostatní
 *             uzly si jen posouvají hodiny, operaci neznají ani nedrží
 *             (PROPOSE nese jen hodiny – jako tab. 7.4 originálu).
 *   DB   Tdb  – logické DB zápisy; provádějí se na VŠECH uzlech v pořadí
 *             klíče. PROPOSE nese celou operaci, příjemce ji drží ve
 *             frontě jako nepotvrzenou (Skeen) až do COMMIT / ABORT.
 *
 * Lamportovy hodiny jsou v PAYLOADU (u64), ne v hlavičce paketu –
 * timestamp v hlavičce zůstává čítač odesílatele pro párování odpovědí.
 *
 * Pořadí operací v pruhu = klíč (t, op.node, op.incarnation, op.seq),
 * porovnává se lexikograficky. Stejné t může dostat víc operací.
 *
 * Pevné struktury = jeden binární záznam (pravidlo z fsproto.h), big-endian,
 * rozšíření jen přidáním na konec (čtenář kontroluje délku >= minimum):
 *
 *   op_id      (20 B)  u32 node | u64 incarnation | u64 seq
 *   sync_hdr   (36 B)  u32 epoch | u32 lane | op_id | u64 t
 *   dbop       (16 B)  u32 code | u32 n_records | u64 time_ms
 *   arch_hdr   (36 B)  u64 index | u64 t | op_id
 *   sync_state (12 B)  u32 state | u64 t
 *   sync_clock (32 B)  u64 tcli | u64 tdb | u64 last_index | u64 db_pending
 *
 * „DB operace“ = [dbop] + n_records záznamů původního DB požadavku beze
 * změny (např. DB_MKNODE: [dbop code=101 n=2 time][path][mknode]).
 *   time_ms  čas určený PŮVODCEM (mtime/ctime nového uzlu) – ne DB
 *   id       nového uzlu stromu odvodí DB z indexu v logu při APPLY
 *            (stejný na všech uzlech, žádný auto-increment)
 * Zápisy jsou PODMÍNĚNÉ: podmínky (rodič existuje, jméno ne…) vyhodnotí
 * DB až při APPLY, takže výsledek je na všech uzlech stejný.
 *
 * „Archivovaná operace“ (MESSAGE_LOGS, DB_APPLY, SYNC_FETCH) =
 *   [arch_hdr] + DB operace. index = pořadové číslo provedené DB
 *   operace (1, 2, 3…), na všech uzlech stejné a souvislé.
 *
 * ------------------------------------------------------------------
 * Kódy (definice v enum kivfs_request_code, protocol.h):
 *
 * Sync <-> Sync jiných uzlů (300–309), všechny začínají [sync_hdr]:
 *   SYNC_PROPOSE 300  req [sync_hdr t=Ttmp] (+ DB operace, jen pruh DB)
 *                     resp [ref][u64 t_navrzene]
 *                     příjemce: t_navrzene = max(t_in, T + 1), T = t_navrzene,
 *                     u pruhu DB operaci zařadí jako PROPOSED
 *   SYNC_COMMIT  301  req [sync_hdr t=T_final]           resp [ref]
 *                     všichni: T = max(T, T_final); DB: PROPOSED -> COMMITTED
 *   SYNC_ABORT   302  req [sync_hdr]                     resp [ref]
 *                     DB: zahodit PROPOSED; CLI: nic (nic se nedrží)
 *   SYNC_QUERY   303  req [sync_hdr]                     resp [ref][sync_state]
 *                     stav operace u příjemce (pro úklid při změně pohledu)
 *   SYNC_CLOCK   304  req []                             resp [ref][sync_clock]
 *                     ověření po startu Syncu (sync.h, „START A OVĚŘENÍ“):
 *                     last_index + počet operací v pruhu DB (db_pending);
 *                     později obnova krok 1 (7.4.3): kdo je nejdál
 *   SYNC_FETCH   305  req [u64 from_index][u32 max]      resp [ref][archivovaná op]...
 *                     obnova krok 2: operace od from_index (po stránkách)
 *
 * DB -> Sync stejného uzlu (310):
 *   SYNC_SUBMIT  310  req [DB operace]                   resp [ref] + výsledek APPLY
 *                     time_ms vyplní DB původce; Sync přidělí op_id a spustí
 *                     kolo Tdb; odpovídá až po lokálním APPLY, stav = stav APPLY
 *
 * Sync -> DB stejného uzlu (110–112):
 *   DB_APPLY     110  req [archivovaná op]               resp [ref] + výsledek
 *                     provede op A zapíše ji do MESSAGE_LOGS v JEDNÉ transakci
 *                     (i se stavem výsledku). Invariant (db/backend.h):
 *                       INTERNAL = nic se nezměnilo ani nezapsalo (rozbitý
 *                         rámec, selhání úložiště, index != head + 1)
 *                       jiný stav = index spotřebován, operace v archivu
 *                         (OK, EXISTS, NOT_FOUND…; vadný obsah = MALFORMED)
 *                     index == head se stejným op_id = opakování (ztracená
 *                     odpověď) -> uložený výsledek, nic se neprovede;
 *                     MKNODE OK -> [attr]
 *   DB_LOG_READ  111  req [u64 from_index][u32 max]      resp [ref][archivovaná op]...
 *                     from_index >= 1 (0 -> MALFORMED); stránka končí, když by
 *                     se další operace nevešla (aspoň 1 se vejde – RESERVE)
 *   DB_LOG_HEAD  112  req []                             resp [ref][u64 last_index]
 *
 * Stavy (enum kivfs_status, protocol.h):
 *   TIMEOUT       -12  výsledek NEZNÁMÝ (operace mohla proběhnout)
 *   VIEW_MISMATCH -13  epocha pohledu nesedí / odesílatel není členem
 *                      pohledu (i stará incarnation); zatím epoch = 1
 *
 * Hrany DB -> SYNC (SUBMIT) a SYNC -> DB (APPLY, LOG_*) jsou
 * v kivfs_module_may_call; kaskáda STATUS jde zvlášť po stromu
 * kivfs_module_status_child (hello.h), takže se nezacyklí.
 * Až se zprávy začnou používat, zvýšit KIVFS_PROTO_VERSION na 2.
 * ------------------------------------------------------------------
 */

#include <stdint.h>
#include "proto/packet.h"

#define KIVFS_OP_ID_WIRE_SIZE      20u
#define KIVFS_SYNC_HDR_WIRE_SIZE   36u
#define KIVFS_DBOP_WIRE_SIZE       16u
#define KIVFS_ARCH_HDR_WIRE_SIZE   36u
#define KIVFS_SYNC_STATE_WIRE_SIZE 12u
#define KIVFS_SYNC_CLOCK_WIRE_SIZE 32u

/* Rezerva payloadu, kterou si nechává SUBMIT: archivovaná operace
 * (+ arch_hdr, + ref, + hlavičky záznamů) se pak vždy vejde do jedné
 * odpovědi SYNC_FETCH / DB_LOG_READ. SUBMIT větší než
 * KIVFS_MAX_PAYLOAD - KIVFS_SYNC_PAYLOAD_RESERVE -> MALFORMED. */
#define KIVFS_SYNC_PAYLOAD_RESERVE 256u

/* Max. počet operací v jedné stránce FETCH / LOG_READ (odpovídající
 * vrátí méně, když se nevejdou do KIVFS_MAX_PAYLOAD; vždy aspoň 1,
 * pokud nějaká od from_index existuje). */
#define KIVFS_SYNC_FETCH_MAX       1024u

enum kivfs_sync_lane {
    KIVFS_LANE_CLI = 0,
    KIVFS_LANE_DB  = 1,
    KIVFS_LANE_COUNT
};

/* Stav operace u uzlu (odpověď na SYNC_QUERY). */
enum kivfs_sync_op_state {
    KIVFS_OPST_UNKNOWN   = 0,   /* operaci nezná (nedostal PROPOSE, nebo zahozena) */
    KIVFS_OPST_PROPOSED  = 1,   /* má PROPOSE, nemá COMMIT; t = jeho návrh */
    KIVFS_OPST_COMMITTED = 2,   /* má COMMIT, ještě neprovedena; t = T_final */
    KIVFS_OPST_APPLIED   = 3    /* provedena (je v MESSAGE_LOGS); t = T_final */
};

/* Globálně jednoznačná identita operace: původce, jeho běh, pořadí v běhu.
 * incarnation je z HELLO – po restartu seq začíná znovu od 1. */
struct kivfs_op_id {
    uint32_t node;
    uint64_t incarnation;
    uint64_t seq;
};

struct kivfs_sync_hdr {
    uint32_t           epoch;   /* epocha pohledu odesílatele (zatím 1) */
    uint32_t           lane;    /* enum kivfs_sync_lane */
    struct kivfs_op_id op;
    uint64_t           t;       /* význam podle zprávy: Ttmp / T_final / 0 */
};

struct kivfs_dbop {
    uint32_t code;              /* původní kód DB požadavku (DB_MKNODE…) */
    uint32_t n_records;         /* počet záznamů operace, které následují */
    uint64_t time_ms;           /* čas od původce */
};

struct kivfs_arch_hdr {
    uint64_t           index;   /* pořadí provedené DB operace, od 1 */
    uint64_t           t;       /* T_final */
    struct kivfs_op_id op;
};

struct kivfs_sync_state {
    uint32_t state;             /* enum kivfs_sync_op_state */
    uint64_t t;
};

struct kivfs_sync_clock {
    uint64_t tcli, tdb;
    uint64_t last_index;        /* poslední provedená DB operace (0 = žádná) */
    uint64_t db_pending;        /* operací v pruhu DB (rozpracované kola,
                                 * dohodnuté, prováděné) */
};

/* Porovnání klíčů pořadí (t, node, incarnation, seq): <0, 0, >0. */
static inline int kivfs_sync_key_cmp(uint64_t ta, const struct kivfs_op_id *a,
                                     uint64_t tb, const struct kivfs_op_id *b)
{
    if (ta != tb)                         return ta < tb ? -1 : 1;
    if (a->node != b->node)               return a->node < b->node ? -1 : 1;
    if (a->incarnation != b->incarnation) return a->incarnation < b->incarnation ? -1 : 1;
    if (a->seq != b->seq)                 return a->seq < b->seq ? -1 : 1;
    return 0;
}

static inline int kivfs_op_id_eq(const struct kivfs_op_id *a, const struct kivfs_op_id *b)
{
    return a->node == b->node && a->incarnation == b->incarnation && a->seq == b->seq;
}

/* Přidání na konec paketu: KIVFS_OK / KIVFS_ENOMEM / KIVFS_ETOOBIG.
 * (op_id se samostatně neposílá – je součástí sync_hdr a arch_hdr.) */
int kivfs_sync_hdr_add  (struct kivfs_packet *p, const struct kivfs_sync_hdr *h);
int kivfs_dbop_add      (struct kivfs_packet *p, const struct kivfs_dbop *d);
int kivfs_arch_hdr_add  (struct kivfs_packet *p, const struct kivfs_arch_hdr *a);
int kivfs_sync_state_add(struct kivfs_packet *p, const struct kivfs_sync_state *s);
int kivfs_sync_clock_add(struct kivfs_packet *p, const struct kivfs_sync_clock *c);

/* Čtení dalšího záznamu: KIVFS_OK, KIVFS_EEND, KIVFS_EMALFORMED (krátký
 * záznam; u sync_hdr i lane >= KIVFS_LANE_COUNT). Při chybě se reader
 * neposune. Obsah (epocha, incarnation…) nekontrolují – to je logika Syncu. */
int kivfs_sync_hdr_read  (struct kivfs_pkt_reader *r, struct kivfs_sync_hdr *h);
int kivfs_dbop_read      (struct kivfs_pkt_reader *r, struct kivfs_dbop *d);
int kivfs_arch_hdr_read  (struct kivfs_pkt_reader *r, struct kivfs_arch_hdr *a);
int kivfs_sync_state_read(struct kivfs_pkt_reader *r, struct kivfs_sync_state *s);
int kivfs_sync_clock_read(struct kivfs_pkt_reader *r, struct kivfs_sync_clock *c);

/* Přeskočí jednu DB operaci ([dbop] + n_records záznamů) a vrátí ukazatel
 * na její začátek v paketu a délku v bajtech (vč. hlaviček záznamů) –
 * Sync ji tak přenese (PROPOSE -> fronta -> DB_APPLY) bez rozbalování.
 * KIVFS_OK, KIVFS_EEND, KIVFS_EMALFORMED (záznamů je méně než n_records). */
int kivfs_dbop_skip(struct kivfs_pkt_reader *r, const uint8_t **start, uint32_t *len);

/* Přečte jednu DB operaci z r a připojí ji na konec dst (záznam po
 * záznamu, beze změny). r může být i reader nad kopií uloženou po
 * kivfs_dbop_skip: struct kivfs_pkt_reader r = { kopie, len, 0 }.
 *   KIVFS_OK; KIVFS_EEND / KIVFS_EMALFORMED – r se neposune, dst beze změny;
 *   KIVFS_ENOMEM / KIVFS_ETOOBIG (z dst) – v dst může zůstat ČÁST operace,
 *   volající takový paket zahodí (pozice r je pak nedefinovaná). */
int kivfs_dbop_copy(struct kivfs_packet *dst, struct kivfs_pkt_reader *r);

#endif /* KIVFS_SYNCPROTO_H */
