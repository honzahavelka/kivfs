#ifndef KIVFS_EVENTS_H
#define KIVFS_EVENTS_H

/*
 * KIVFS – fronta událostí modulu
 * ==============================
 *
 * Spojka mezi reactorem (I/O vlákno) a workerem modulu. Každý modul má
 * právě jednu instanci:
 *
 *   reactor --callbacky--> kivfs_ev (fronta) --kivfs_ev_pop--> worker
 *      ^                                                         |
 *      +------ kivfs_ev_request / _connect / _timer -------------+
 *              (přímo: kivfs_reactor_send, kivfs_reactor_close_conn)
 *
 * Callbacky reactoru vyplní kivfs_ev_callbacks – modul je nepíše sám.
 *
 * HLAVNÍ PROBLÉM, který tento modul řeší: některé události se NESMÍ
 * zahodit. Příchozí požadavek odmítnout lze (odpověď BUSY), ale ztracený
 * výsledek odchozího požadavku by navždy zasekl operaci, která na něj čeká
 * (porušil by se invariant "každý request právě jeden on_response").
 *
 * Řešení – účtování po třídách. Každá událost patří do třídy s limitem;
 * fronta má kapacitu = součet limitů, takže se nikdy nenaplní dřív, než
 * některá třída dosáhne limitu:
 *
 *   třída              událost                  při plné třídě
 *   ----------------   ----------------------   ------------------------------
 *   REQUEST            příchozí požadavek       odpověď BUSY (v I/O vlákně)
 *   IN_CLOSE           zánik příchozího spoj.   zahodí se (viz níže)
 *   RESPONSE   [G]     výsledek kivfs_ev_request   nemůže nastat
 *   OUT_CONN   [G]     connect + zánik odchozího   nemůže nastat
 *                      (1 jednotka = 1 spojení = 2 místa ve frontě)
 *   TIMER      [G]     uplynulý časovač            nemůže nastat
 *   USER       [G]     kivfs_ev_post               nemůže nastat
 *
 * [G] = garantovaná třída. Garance plyne z toho, že limit se kontroluje
 * UŽ PŘI VZNIKU (kivfs_ev_request/_connect/_timer/_post vrátí KIVFS_EAGAIN)
 * a jednotka limitu se uvolní až ve chvíli, kdy worker událost VYZVEDNE
 * (kivfs_ev_pop). Nestačilo by spoléhat na limity reactoru: ten uvolní
 * místo už při callbacku, takže by worker mohl vyrábět nové požadavky
 * rychleji, než vybírá jejich výsledky, a fronta by rostla bez omezení.
 * Kdyby se garantovaná událost přesto nevešla, je to chyba v programu
 * -> log + abort() (raději hlasitý pád než tiše visící operace).
 *
 * Proč IN_CLOSE smí ztrácet: sloty příchozích spojení se po zániku hned
 * znovu používají, takže při rychlém střídání spojení (connect/close
 * smyčka protistrany) by počet těchto událostí nebyl omezený. Modul má
 * stav příchozích spojení indexovaný slotem a ověřený celým conn_id, takže
 * zahozený IN_CLOSE jen nechá starý záznam, který se přepíše při dalším
 * HELLO na stejném slotu. Odpověď na mrtvé spojení reactor zahodí sám.
 * Ze stejného důvodu se on_accept vůbec nepřevádí na událost – modul
 * se o příchozím spojení dozví z HELLO.
 *
 * Totéž platí pro REQUEST: požadavek mrtvého spojení může ve frontě
 * zůstat, zatímco slot už používá nové spojení – proto ani REQUEST není
 * shora omezený jen počtem spojení a musí mít vlastní limit.
 *
 * Pořadí: jedna FIFO pro všechny třídy, takže se zachová pořadí, v jakém
 * reactor callbacky volal (u odchozího spojení: všechny RESPONSE před
 * jeho CLOSE).
 *
 * Vlákna: zatím JEDEN worker na modul. Rozšíření na víc workerů
 * (route(událost) -> index workeru, odpověď vlastníkovi podle tagu) se
 * udělá tady, uvnitř callbacků – rozhraní pro worker se nezmění.
 *
 * Implementace: kivfs_queue (kapacita = součet limitů) + čítače tříd pod
 * vlastním mutexem. Callbacky reactoru se volají bez držení zámku
 * mailboxu, takže pořadí zámků je vždy ev -> (žádný) a reactor -> (žádný).
 *
 * Návratové kódy: errors.h.
 */

#include <stdint.h>
#include <sys/socket.h>
#include "proto/packet.h"
#include "core/reactor.h"

/* ------------------------------------------------------------------ */
/* Událost                                                             */
/* ------------------------------------------------------------------ */

enum kivfs_ev_type {
    KIVFS_EV_REQUEST = 1, /* příchozí požadavek: conn, pkt.
                           * Odpovědět přes kivfs_reactor_send(conn). */
    KIVFS_EV_RESPONSE,    /* výsledek kivfs_ev_request: conn, tag, err;
                           * pkt jen při err == KIVFS_OK */
    KIVFS_EV_CONNECT,     /* odchozí spojení navázáno: conn */
    KIVFS_EV_CLOSE,       /* spojení zaniklo: conn, outgoing, reason, err.
                           * Odchozí: vždy doručeno a vždy až po všech jeho
                           * RESPONSE. Příchozí: best-effort (viz výše). */
    KIVFS_EV_TIMER,       /* uplynul časovač: tag */
    KIVFS_EV_USER         /* kivfs_ev_post: tag (např. START) */
};

struct kivfs_event {
    enum kivfs_ev_type      type;
    kivfs_conn_id           conn;
    uint64_t                tag;
    int                     err;      /* RESPONSE: errors.h; CLOSE: errno nebo 0 */
    enum kivfs_close_reason reason;   /* CLOSE */
    int                     outgoing; /* CLOSE: 1 = odchozí spojení */
    struct kivfs_packet     pkt;      /* REQUEST; RESPONSE s err == KIVFS_OK;
                                       * jinak prázdný (data == NULL).
                                       * Událost paket VLASTNÍ. */
};

/* Uvolní paket události (idempotentní). Worker volá po zpracování každé
 * události, pokud si paket nepřevzal (např. do rozpracované operace –
 * pak e->pkt vynuluje). */
void kivfs_ev_event_free(struct kivfs_event *e);

/* ------------------------------------------------------------------ */
/* Životní cyklus                                                      */
/* ------------------------------------------------------------------ */

/* Limity tříd; 0 = výchozí hodnota v komentáři. */
struct kivfs_ev_limits {
    uint32_t max_requests;    /* příchozí požadavky ve frontě (výchozí = max_conns reactoru) */
    uint32_t max_in_closes;   /* zániky příchozích spojení (výchozí = max_conns) */
    uint32_t max_outstanding; /* rozpracované odchozí požadavky – od
                               * kivfs_ev_request do vyzvednutí RESPONSE (výchozí 256) */
    uint32_t max_out_conns;   /* odchozí spojení – od kivfs_ev_connect do
                               * vyzvednutí CLOSE (výchozí = max_out_conns reactoru) */
    uint32_t max_timers;      /* časovače – od kivfs_ev_timer do vyzvednutí TIMER (výchozí 256) */
    uint32_t max_user;        /* kivfs_ev_post (výchozí 16) */
};

struct kivfs_ev;

/* Vytvoří frontu. lim smí být NULL (vše výchozí); rcfg slouží jen
 * k odvození výchozích limitů.
 * KIVFS_EINVAL, KIVFS_ENOMEM. */
int  kivfs_ev_create(struct kivfs_ev **out, const struct kivfs_ev_limits *lim,
                     const struct kivfs_reactor_config *rcfg);

/* Vyplní callbacky reactoru (všechny, user = ev). Použití:
 *     kivfs_ev_callbacks(ev, &cb);
 *     kivfs_reactor_create(&r, &rcfg, &cb);
 *     kivfs_ev_attach(ev, r);          // PŘED kivfs_reactor_run
 * Reactor je potřeba kvůli odpovědi BUSY a kvůli wrapperům níže. */
void kivfs_ev_callbacks(struct kivfs_ev *ev, struct kivfs_reactor_callbacks *cb);
void kivfs_ev_attach(struct kivfs_ev *ev, struct kivfs_reactor *r);

/* Vyzvedne nejstarší událost; je-li fronta prázdná, ČEKÁ. Uvolní
 * jednotku limitu třídy události. Po kivfs_ev_shutdown vydá zbytek
 * (drain) a pak vrací KIVFS_ESHUTDOWN -> worker končí. */
int  kivfs_ev_pop(struct kivfs_ev *ev, struct kivfs_event *out);

/* Pořadí při vypínání (reactor ještě v run posílá ESHUTDOWN a CLOSE,
 * fronta je musí přijmout):
 *   kivfs_reactor_stop -> návrat run -> kivfs_ev_shutdown -> join workera
 *   -> kivfs_reactor_destroy -> kivfs_ev_destroy */
void kivfs_ev_shutdown(struct kivfs_ev *ev);
void kivfs_ev_destroy(struct kivfs_ev *ev);

/* ------------------------------------------------------------------ */
/* Akce, které vyrobí budoucí událost – volá worker                    */
/* Obalují stejnojmenné funkce reactoru a navíc rezervují místo ve     */
/* frontě. Modul NESMÍ volat kivfs_reactor_request/_connect/_timer      */
/* přímo – obešel by účtování a garance by neplatila.                   */
/* Vlastnictví paketu a sémantika jsou stejné jako u reactoru.          */
/* Navíc: KIVFS_EAGAIN = vyčerpán limit třídy (žádná událost nepřijde). */
/* ------------------------------------------------------------------ */

int  kivfs_ev_connect(struct kivfs_ev *ev, const struct sockaddr *addr, socklen_t addrlen,
                      kivfs_conn_id *out);
int  kivfs_ev_request(struct kivfs_ev *ev, kivfs_conn_id id, struct kivfs_packet *req,
                      uint64_t tag, int timeout_ms);
int  kivfs_ev_timer  (struct kivfs_ev *ev, int delay_ms, uint64_t tag);

/* Vloží KIVFS_EV_USER s tagem (např. START po spuštění, nebo práce, kterou
 * si worker naplánuje "na později"). Smí volat kterékoli vlákno. */
int  kivfs_ev_post   (struct kivfs_ev *ev, uint64_t tag);

#endif /* KIVFS_EVENTS_H */
