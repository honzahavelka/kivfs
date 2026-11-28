#ifndef KIVFS_SYNC_H
#define KIVFS_SYNC_H

/*
 * KIVFS – modul Sync: vnitřní stav a rozdělení práce (25. 11.)
 * ===================================================================
 *
 * Nad dvěma pruhy (lane.h) a zprávami (syncproto.h) řeší Sync to, co
 * pruh záměrně neumí: síť, kola hlasování, timeouty, spolehlivé doručení
 * COMMIT/ABORT a úklid po restartu původce. Vše běží ve workeru Syncu,
 * bez zámků. Rozhodnutí: claude/architektura.md (25. 11.).
 *
 * Pruh CLI: klientské operace (kolo Tcli, provádění přes VFS u původce).
 * Pruh DB (26. 11.): zápisy metadat – SYNC_SUBMIT od DB -> kolo Tdb ->
 * DB_APPLY na VŠECH uzlech v pořadí Tdb (apply.c). Obnova uzlu (CLOCK/
 * FETCH podle indexu) zatím chybí – uzel, kterému chybí operace, se
 * zastaví (STOPPED).
 *
 * ------------------------------------------------------------------
 * KOLO U PŮVODCE (sync_round)
 *   1. kivfs_lane_begin -> položka PROPOSED, Ttmp
 *   2. SYNC_PROPOSE všem ostatním uzlům (kivfs_mod_call_node),
 *      timeout = round_timeout; tmax = Ttmp
 *   3. každá odpověď: OK -> tmax = max(tmax, t); jinak zapamatovat chybu
 *   4. poslední odpověď (waiting == 0):
 *        vše OK -> kivfs_lane_commit(tmax), SYNC_COMMIT všem (outbox),
 *                  sync_execute_next
 *        chyba  -> kivfs_lane_abort, SYNC_ABORT všem (outbox), odpověď
 *                  klientovi podle tabulky níže
 *   Pevný pohled (krok 1): odpovědět musí VŠICHNI ostatní uzly.
 *
 * Chyba kola -> stav pro klienta. Sync ví jistě, že operace NEPROBĚHLA
 * (bez COMMIT ji nikdo neprovede), proto nikdy TIMEOUT:
 *   linka dole (ENOTREADY), timeout, pád spojení, VIEW_MISMATCH,
 *   MALFORMED/INTERNAL od příjemce                 -> UNAVAILABLE
 *   BUSY od příjemce, plný pruh/tabulka volání    -> BUSY
 *
 * PROVÁDĚNÍ (pruh CLI, jen u původce): sériově v pořadí Tcli (originál)
 *   kivfs_lane_next -> Sync -> VFS (kivfs_mod_call, timeout vfs_timeout)
 *   -> odpověď VFS přebalit pro Auth -> kivfs_lane_finish -> další.
 *   Timeout / pád spojení s VFS: výsledek NEZNÁMÝ (VFS mohl zapsat)
 *   -> klientovi TIMEOUT, finish se zavolá i tak (pruh CLI nemá stav
 *   k rozbití – jde jen o pořadí u původce).
 *   order_cli = no: kolo ani pruh se nepoužijí, požadavek jde rovnou do
 *   VFS (dnešní kivfs_mod_forward).
 *
 * ------------------------------------------------------------------
 * PŘÍJEMCE (zprávy od Synců jiných uzlů)
 *   Společné kontroly každé zprávy, v tomto pořadí:
 *     sync_hdr čitelný a lane platný                  jinak MALFORMED
 *     epoch == sync_state.epoch (zatím vždy 1)        jinak VIEW_MISMATCH
 *     PROPOSE/ABORT: op.node == from->node a
 *       op.incarnation == from->incarnation           jinak MALFORMED (+ log)
 *   PROPOSE: pruh DB – zbytek paketu musí být právě jedna DB operace
 *     (kivfs_dbop_skip, pak konec paketu); neověřený Sync (po startu)
 *     odpoví UNAVAILABLE (viz „START A OVĚŘENÍ“); kivfs_lane_on_propose;
 *     EAGAIN -> BUSY; odpověď [ref][u64 t].
 *   COMMIT:  kivfs_lane_commit; ENOENT u pruhu CLI = normální (OK);
 *     ENOENT u pruhu DB a není v recent = operaci neznám -> sync_db_stop
 *     (STOPPED), odpovědět OK (COMMIT jsem dostal); EPROTO -> INTERNAL + log.
 *   ABORT:   kivfs_lane_abort; ENOENT = OK (neviděl PROPOSE / duplicitní
 *     ABORT); EPROTO -> INTERNAL + log.
 *   QUERY:   stav z pruhu (find), jinak z recent, jinak UNKNOWN.
 *   CLOCK:   [sync_clock] (tcli, tdb, last_index, db_pending = počet
 *     položek pruhu DB); head DB ještě neznám -> UNAVAILABLE.
 *   Odpověď na zprávu Sync<->Sync se posílá HNED – nikdy nečeká na frontu
 *   (pravidlo proti logickému deadlocku).
 *
 * ------------------------------------------------------------------
 * SPOLEHLIVÉ COMMIT/ABORT (sync_outbox)
 *   Původce rozhodl -> rozhodnutí MUSÍ dorazit všem, jinak u příjemce
 *   visí PROPOSED a blokuje mu pruh DB. Proto se COMMIT/ABORT neposílá
 *   „jednou“, ale přes frontu na každý uzel: odeslání přes call_node,
 *   položka zmizí až po odpovědi (podle id – odpověď na pozdější zprávu
 *   nesmí odebrat dřívější, která vypršela); chyba -> pošle se znovu po
 *   další odpovědi, linka dole -> při KIVFS_PEER_LINK_UP toho uzlu.
 *   V letu je nejvýš SYNC_OUTBOX_INFLIGHT zpráv na uzel (odchozí fronta
 *   spojení má jen max_pending míst a sdílí ji s PROPOSE). Příjemce je na
 *   opakování připravený (commit se stejným t = OK, abort neznámé = OK).
 *   Pořadí mezi různými operacemi nevadí (COMMIT je idempotentní,
 *   stabilitu drží pruh).
 *   Místo pro rozhodnutí se REZERVUJE při startu kola (count + reserved
 *   < SYNC_OUTBOX_CAP u všech uzlů, jinak kolo BUSY) – rozhodnutí kola,
 *   které už proběhlo, se tak nikdy neztratí kvůli plné frontě.
 *
 * ------------------------------------------------------------------
 * RESTART PŮVODCE (sync_resolve)
 *   on_peer s novou incarnation uzlu X: jeho starý běh už COMMIT ani
 *   ABORT nepošle (outbox zemřel s ním). Pro každou PROPOSED položku od
 *   (X, stará incarnation) – pruh DB (CLI cizí položky nedrží):
 *     SYNC_QUERY všem ostatním uzlům KROMĚ X
 *     kdokoli COMMITTED/APPLIED s t   -> kivfs_lane_commit(t)
 *     všichni odpověděli, nikdo nemá  -> kivfs_lane_abort (zahodit)
 *     někdo neodpověděl               -> zkusit znovu při jeho LINK_UP
 *   Zahodit smí jen když odpověděli VŠICHNI – jinak by se repliky rozešly.
 *
 * ------------------------------------------------------------------
 * KONFIGURACE
 *   [cluster] op_timeout_ms = 30000   časový rámec jedné operace (7.4.3:
 *             Timeout, výchozí 30 s); v [cluster], protože ho potřebuje
 *             i Auth (čeká na Sync) – [cluster] čtou všechny moduly
 *   [sync]    order_cli = yes|no      kolo Tcli pro klientské operace
 *   Odvozené timeouty: kivfs_modconf_timeouts (modconf.h) – celý řetěz
 *   Auth > Sync > VFS > DB > kolo na jednom místě. Sync používá:
 *     round_timeout = 2/16 O   kolo PROPOSE, outbox, QUERY, CLOCK
 *     vfs_timeout   = 8/16 O   Sync -> VFS
 *     apply_timeout = 1/16 O   DB_APPLY, DB_LOG_HEAD (vypršení = zopakovat)
 */

#include <stdint.h>
#include "core/module.h"
#include "proto/syncproto.h"
#include "sync/lane.h"

#define SYNC_OUTBOX_CAP    4096u   /* nedoručených COMMIT/ABORT na uzel */
#define SYNC_OUTBOX_INFLIGHT 4u    /* z nich odeslaných a čekajících na odpověď */
#define SYNC_RECENT_CAP    4096u   /* pamatovaných dokončených operací */
#define SYNC_LANE_ENTRIES  1024u   /* max_entries každého pruhu */
#define SYNC_LANE_BYTES    (256u * 1024u * 1024u)  /* max_bytes pruhu DB */
#define SYNC_DB_RETRY_MS   200u    /* další pokus o LOG_HEAD / ověření / APPLY */

/* ------------------------------------------------------------------
 * PRUH DB U PŮVODCE (origin.c)
 *   SYNC_SUBMIT od DB stejného uzlu: payload = právě jedna DB operace
 *   (jinak MALFORMED), velikost <= MAX_PAYLOAD - RESERVE; db.mode musí být
 *   RUNNING (jinak UNAVAILABLE – jistě neprovedeno); outbox_reserve
 *   (BUSY); kivfs_lane_begin(DB, payload, sync_db_op) -> kolo jako u CLI.
 *   Chyba kola -> odpověď na SUBMIT UNAVAILABLE / BUSY (tabulka výše).
 *   Úspěch kola (bod 13 a): kivfs_lane_hold(n ostatních uzlů) PŘED
 *   commit; outbox při odpovědi na COMMIT vlastní DB operace volá
 *   sync_commit_acked -> release. Odmítnutí COMMIT příjemcem se počítá
 *   jako potvrzení (log „uzel potřebuje obnovu“) – jinak by původce stál
 *   kvůli rozbitému uzlu navždy. Linka dole -> původce čeká (outbox
 *   opakuje při LINK_UP); DB mezitím vrátí VFS TIMEOUT. 1 uzel -> bez hold.
 *   Odpověď na SUBMIT až po vlastním DB_APPLY: stav + záznamy APPLY
 *   (MKNODE [attr]).
 *
 * PROVÁDĚNÍ PRUHU DB (apply.c, na každém uzlu)
 *   Jen v db.mode RUNNING; sériově: kivfs_lane_next (respektuje hold) ->
 *   DB_APPLY [arch_hdr(index, t, op)][DB operace] (timeout apply_timeout).
 *     stav != INTERNAL     index spotřebován (backend.h): finish, recent
 *                          APPLIED, u vlastní odpověď na SUBMIT, další
 *     INTERNAL             díra v posloupnosti -> STOPPED, finish NE
 *     ETIMEOUT/ECONNLOST   APPLY se pošle ZNOVU (hned, resp. po LINK_UP DB)
 *                          – DB opakování poslední operace pozná (R5) a
 *                          vrátí uložený výsledek
 *
 * VZTAH K DB (apply.c): každý LINK_UP na DB -> head neznámý -> DB_LOG_HEAD
 *   před ověřením (start)  head se převezme (kivfs_lane_set_last_index)
 *   po ověření             h == L, nebo h == index rozpracovaného APPLY
 *                          (provedlo se, odpověď se ztratila) -> dál;
 *                          jinak (DB ztratila data / díra) -> STOPPED;
 *                          výjimka: cluster s 1 uzlem a nic se neprovádí
 *                          -> převzít h (DB je jediný zdroj pravdy)
 *
 * START A OVĚŘENÍ (26. 11., přísná varianta): nová inkarnace Syncu nic
 *   neprovede, dokud se u ostatních uzlů nepřesvědčí, že jí nic nechybí
 *   (stará inkarnace mohla hlasovat o operaci, kterou neprovedla):
 *     1. head DB = h (viz výše)
 *     2. SYNC_CLOCK všem ostatním: každý má last_index == h a db_pending
 *        == 0 -> ověřeno (RUNNING)
 *        kdokoli last_index > h -> chybí mi operace -> STOPPED
 *        jinak (rozdíl, rozpracované, neodpověděl, linka dole) -> zkusit
 *        znovu za SYNC_DB_RETRY_MS
 *   Ztracená operace je vždy aspoň u svého původce – v pruhu (pending),
 *   nebo v archivu s indexem > h. Aby srovnání dávalo smysl, NEOVĚŘENÝ
 *   Sync odmítá PROPOSE pruhu DB (UNAVAILABLE): nová DB operace se bez
 *   jeho hlasu nedohodne, takže v pruzích ostatních nemůže být nic,
 *   o čem by nevěděl. Pruh CLI se neomezuje. Cluster s 1 uzlem = ověřeno.
 *   SYNC_CLOCK u neověřeného s neznámým head -> UNAVAILABLE (tazatel to
 *   zkusí znovu).
 *   COMMIT neznámé DB operace (není v pruhu ani v recent) -> STOPPED
 *   (dřív jen log): uzel ji neprovede a další index by patřil jiné operaci.
 *
 * STOPPED: neprovádí, SUBMIT -> UNAVAILABLE, čekajícím vlastním SUBMITům
 *   hned TIMEOUT (operace jsou dohodnuté, proběhnou jinde). Dál hlasuje
 *   a řadí (při plném pruhu BUSY). Ven jen restartem Syncu (pak ověření)
 *   – obnova z archivu ostatních je další krok.
 * ------------------------------------------------------------------ */

/* Kontext klientské operace u původce = user položky pruhu CLI. */
struct sync_cli_op {
    kivfs_conn_id       from;      /* spojení od Auth, kterému odpovědět */
    struct kivfs_packet req;       /* původní požadavek (vlastní, kvůli ref
                                    * a payloadu pro VFS) */
};

/* Kontext vlastní DB operace u původce = user položky pruhu DB.
 * Odpověď na SUBMIT potřebuje jen hlavičku požadavku (ref); DB operaci
 * si drží pruh (payload). */
struct sync_db_op {
    kivfs_conn_id       from;      /* spojení od DB */
    struct kivfs_header orig;      /* hlavička SYNC_SUBMIT */
};

/* Vztah Syncu k lokální DB (apply.c). */
enum sync_db_mode {
    SYNC_DB_UNKNOWN = 0,   /* neprovádím: linka dole, head neznámý, nebo
                            * ještě neověřeno po startu */
    SYNC_DB_RUNNING = 1,   /* provádím */
    SYNC_DB_STOPPED = 2    /* chybí operace – potřebuji obnovu; trvalé */
};

struct sync_db {
    uint32_t                 mode;           /* enum sync_db_mode */
    int                      link_up;        /* moje linka Sync -> DB */
    int                      head_ok;        /* head DB zjištěn od posl. LINK_UP */
    int                      head_known;     /* head zjištěn aspoň jednou od startu */
    int                      head_inflight;  /* běží DB_LOG_HEAD */
    int                      verified;       /* ověření po startu prošlo */
    int                      check_inflight; /* běží kolo SYNC_CLOCK */
    int                      retry_armed;    /* běží časovač dalšího pokusu */
    struct kivfs_lane_entry *exec;           /* vydaná hlava (EXECUTING), nebo NULL */
    int                      exec_inflight;  /* její DB_APPLY čeká na odpověď */
};

/* Kolo hlasování u původce; state všech jeho kivfs_mod_call_node.
 * Žije, dokud nepřijdou všechny cont (waiting == 0). */
struct sync_round {
    uint32_t           lane;
    struct kivfs_op_id op;         /* položku hledám přes find – ukazatel
                                    * neukládám (abort/drop ji může zrušit) */
    uint64_t           tmax;
    uint32_t           waiting;    /* nevrácené odpovědi PROPOSE */
    int16_t            fail;       /* 0 = zatím OK, jinak stav pro klienta
                                    * (BUSY má přednost před UNAVAILABLE) */
};

/* Jedna nedoručená zpráva v outboxu. */
struct sync_out_msg {
    uint64_t           id;         /* jednoznačné v rámci outboxu (párování odpovědi) */
    uint8_t            done;       /* potvrzená, čeká jen na odebrání z čela */
    uint16_t           code;       /* KIVFS_REQ_SYNC_COMMIT / _ABORT */
    uint32_t           lane;
    struct kivfs_op_id op;
    uint64_t           t;          /* T_final u COMMIT */
    uint8_t            inflight;   /* odesláno, čeká se na odpověď */
};

/* Kruhová fronta zpráv pro jeden uzel. */
struct sync_outbox {
    struct sync_out_msg *q;        /* [SYNC_OUTBOX_CAP] */
    uint32_t             head, count;
    uint32_t             reserved;  /* místa rezervovaná rozpracovanými koly
                                     * (count + reserved <= SYNC_OUTBOX_CAP) */
    uint32_t             inflight;  /* odeslaných bez odpovědi */
    uint64_t             next_id;
};

/* Dokončená operace (pro SYNC_QUERY a pozdní duplicity). */
struct sync_recent {
    uint32_t           lane;
    struct kivfs_op_id op;
    uint32_t           state;      /* KIVFS_OPST_APPLIED nebo _UNKNOWN (abort) */
    uint64_t           t;
};

/* Úklid jedné PROPOSED položky po restartu jejího původce. */
struct sync_resolve {
    uint32_t           lane;
    struct kivfs_op_id op;
    uint32_t           waiting;    /* nevrácené QUERY */
    uint32_t           missing;    /* uzly, které neodpověděly / byly dole */
    int                found;      /* někdo má COMMITTED/APPLIED */
    uint64_t           t;          /* jeho T_final */
    uint32_t           origin;     /* uzel, který se restartoval (neptat se ho) */
    struct sync_resolve *next;     /* seznam nedokončených úklidů */
};

/* Jiný uzel z [cluster], jak ho vidí Sync. */
struct sync_node {
    uint32_t           id;
    uint64_t           incarnation;   /* poslední viděná (0 = ještě ne) */
    int                link_up;       /* moje odchozí linka je UP */
    struct sync_outbox out;
};

struct sync_state {
    struct kivfs_mod  *m;
    struct kivfs_lane *lane[KIVFS_LANE_COUNT];
    uint32_t           epoch;         /* pevný pohled: 1 */
    int                order_cli;     /* [sync] order_cli */
    int                op_timeout_ms; /* [cluster] op_timeout_ms */
    int                round_timeout_ms, vfs_timeout_ms;
    int                apply_timeout_ms;  /* DB_APPLY, DB_LOG_HEAD (zatím op/4) */
    uint32_t           self_node;     /* moje node_id (vlastní op: op.node == self) */
    struct sync_db     db;

    struct sync_node   nodes[KIVFS_MAX_NODES];  /* ostatní uzly, bez mě */
    size_t             n_nodes;

    struct sync_recent recent[SYNC_RECENT_CAP]; /* kruhově */
    uint32_t           recent_next;

    struct sync_resolve *resolves;              /* čekající úklidy (seznam) */
};

/* ------------------------------------------------------------------ */
/* Rozdělení do souborů (vše volá jen worker Syncu)                    */
/* ------------------------------------------------------------------ */

/* sync/main.c – init/fini, rozdělení požadavků podle kódu a odesílatele,
 * on_peer (LINK_UP -> sync_outbox_flush; nová incarnation -> sync_resolve_origin) */

/* sync/origin.c – původce */
/* Klientská operace od Auth: order_cli -> kolo v pruhu CLI, jinak forward.
 * Vrací 1 = převzato (odpověď přijde vždy, i chybová). */
int  sync_cli_submit(struct sync_state *st, const struct kivfs_mod_peer *from,
                     struct kivfs_packet *req);
/* Spustí kolo pro právě vytvořenou položku (po kivfs_lane_begin). Při
 * nemožnosti odeslat komukoli kolo rovnou ukončí chybou. */
void sync_round_start(struct sync_state *st, uint32_t lane, const struct kivfs_lane_entry *e);
/* Vydá a spustí provádění dalších operací pruhu, pokud nějaké jsou
 * (pruh DB -> sync_db_execute). */
void sync_execute_next(struct sync_state *st, uint32_t lane);
/* SYNC_SUBMIT od DB stejného uzlu (viz „PRUH DB U PŮVODCE“). Vrací 1. */
int  sync_db_submit(struct sync_state *st, const struct kivfs_mod_peer *from,
                    struct kivfs_packet *req);
/* outbox: uzel potvrdil (nebo odmítl) COMMIT vlastní DB operace ->
 * kivfs_lane_release + provádění. */
void sync_commit_acked(struct sync_state *st, const struct kivfs_op_id *op);
/* Připojí záznamy src za ref (odpověď, skip_ref) nebo od začátku. */
int  sync_copy_records(struct kivfs_packet *dst, const struct kivfs_packet *src, int skip_ref);

/* sync/apply.c – provádění pruhu DB, vztah k DB, ověření po startu */
/* Udělá, co jde: zjistit head DB, ověřit, vydat hlavu / zopakovat APPLY. */
void sync_db_execute(struct sync_state *st);
/* on_peer pro modul DB (moje linka): LINK_UP -> head neznámý, LOG_HEAD;
 * LINK_DOWN -> čekat. */
void sync_db_on_link(struct sync_state *st, enum kivfs_peer_event ev, uint64_t incarnation);
/* -> STOPPED: log s důvodem, čekajícím vlastním SUBMITům TIMEOUT. */
void sync_db_stop(struct sync_state *st, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Odpověď na SUBMIT (stav, záznamy za ref z resp, je-li) a uvolnění op. */
void sync_db_reply(struct sync_state *st, struct sync_db_op *op, int16_t status,
                   const struct kivfs_packet *resp);

/* sync/peer.c – příjemce; každá vrací 1 = převzato (vždy odpoví) */
int  sync_on_propose(struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req);
int  sync_on_commit (struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req);
int  sync_on_abort  (struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req);
int  sync_on_query  (struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req);
int  sync_on_clock  (struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req);

/* sync/outbox.c – spolehlivé COMMIT/ABORT */
/* Rezervuje u všech uzlů místo pro rozhodnutí jednoho kola.
 * 0 = OK, -1 = některý outbox je plný (uzel dlouho nedostupný) -> BUSY. */
int  sync_outbox_reserve(struct sync_state *st);
/* Zařadí zprávu pro uzel (spotřebuje rezervaci) a zkusí odeslat. */
void sync_outbox_push (struct sync_state *st, struct sync_node *n, uint16_t code,
                       uint32_t lane, const struct kivfs_op_id *op, uint64_t t);
/* Pošle vše, co neodešlo (při LINK_UP). */
void sync_outbox_flush(struct sync_state *st, struct sync_node *n);

/* sync/resolve.c – úklid po restartu původce */
void sync_resolve_origin(struct sync_state *st, uint32_t node, uint64_t new_incarnation);
/* Zopakuje nedokončené úklidy čekající na uzel (při jeho LINK_UP). */
void sync_resolve_retry (struct sync_state *st, uint32_t node);

/* recent */
void sync_recent_add (struct sync_state *st, uint32_t lane, const struct kivfs_op_id *op,
                      uint32_t state, uint64_t t);
const struct sync_recent *sync_recent_find(const struct sync_state *st, uint32_t lane,
                                           const struct kivfs_op_id *op);

struct sync_node *sync_node_find(struct sync_state *st, uint32_t id);

#endif /* KIVFS_SYNC_H */
