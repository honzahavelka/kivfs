#ifndef KIVFS_REACTOR_H
#define KIVFS_REACTOR_H

/*
 * KIVFS – epoll reactor pro PŘÍCHOZÍ spojení (jen Linux)
 * ======================================================
 *
 * Tok zpráv:
 *
 *   nové spojení -> accept -> přidělí conn_id -> on_accept()
 *   bajty        -> čtení po kouscích -> celý paket -> on_packet() -> (fronta -> workery)
 *   worker       -> kivfs_reactor_send() -> mailbox + eventfd -> I/O vlákno zapíše
 *   konec        -> on_close(důvod)
 *
 * Celý reactor běží v JEDNOM I/O vlákně (kivfs_reactor_run). Jen toto vlákno
 * sahá na sockety a na tabulku spojení, takže ty nepotřebují zámky. Ostatní
 * vlákna smí volat pouze funkce označené [TS] (thread-safe); ty jen vloží
 * požadavek do mailboxu (chráněného mutexem) a probudí I/O vlákno přes
 * eventfd, který je zaregistrovaný v epollu jako další "socket".
 *
 * Reactor neví nic o frontě, workerech ani Lamportových hodinách – celé
 * pakety předává callbackem a odpovědi přijímá přes kivfs_reactor_send.
 *
 * Pravidlo "jeden požadavek na spojení":
 *   Po doručení paketu (on_packet) reactor z daného spojení PŘESTANE ČÍST,
 *   dokud na něj někdo nepošle odpověď (kivfs_reactor_send s paketem, který
 *   má KIVFS_RESPONSE_FLAG). Důsledky:
 *     - zprávy jednoho spojení se zpracují v pořadí (FIFO, nutné pro Lamporta)
 *     - na jednom spojení nikdy nepracují dva workery najednou
 *     - ve frontě workerů je nejvýš tolik paketů, kolik je spojení
 *       (přirozený backpressure – fronta s kapacitou max_conns nepřeteče)
 *     - ve frontě k odeslání má každé spojení nejvýš jednu odpověď
 *   Pošle-li protistrana víc požadavků najednou, další čekají v bufferu
 *   jádra, nic se neztratí. Pokud worker kvůli chybě nikdy neodpoví,
 *   spojení zamrzne – protistrana ho ukončí svým timeoutem.
 *
 * epoll běží v level-triggered režimu: epoll hlásí socket, dokud v něm
 * jsou data. Je to jednodušší a odolnější než edge-triggered, kde by se
 * muselo vždy číst až do EAGAIN, jinak se na zbylá data "zapomene".
 *
 * Mapování conn_id -> node_id (z HELLO) drží vrstva nad reactorem (Sync),
 * ne reactor: naplní ho při zpracování HELLO a smaže v on_close.
 *
 * Návratové kódy: errors.h.
 */

#include <stdint.h>
#include "packet.h"

/* ------------------------------------------------------------------ */
/* Identifikace spojení                                                */
/* ------------------------------------------------------------------ */

/* Spojení se mimo I/O vlákno identifikuje číslem, NE ukazatelem.
 * Horních 32 bitů = generace, dolních 32 bitů = index slotu v tabulce.
 * Když spojení zanikne a jeho slot se použije pro nové, generace se zvýší.
 * Pozdní odpověď workera na staré conn_id se tak pozná a zahodí – neodejde
 * cizímu klientovi a nesáhne do uvolněné paměti. */
typedef uint64_t kivfs_conn_id;

/* Generace začínají od 1, takže 0 nikdy není platné conn_id. */
#define KIVFS_CONN_ID_INVALID 0u

/* Proč spojení skončilo – pro logy a pro modul Sync (detekce pádu uzlu). */
enum kivfs_close_reason {
    KIVFS_CLOSE_PEER,       /* protistrana zavřela čistě mezi pakety */
    KIVFS_CLOSE_IO_ERROR,   /* chyba socketu nebo konec uprostřed paketu */
    KIVFS_CLOSE_PROTOCOL,   /* špatný magic nebo data_size > KIVFS_MAX_PAYLOAD */
    KIVFS_CLOSE_TIMEOUT,    /* paket nedorazil celý do rx_timeout_ms */
    KIVFS_CLOSE_LOCAL,      /* zavřeno přes kivfs_reactor_close_conn */
    KIVFS_CLOSE_SHUTDOWN    /* reactor se ukončuje */
};

/* ------------------------------------------------------------------ */
/* Callbacky – volají se VŽDY v I/O vlákně                             */
/* Musí být rychlé a nesmí blokovat (žádná DB, disk ani síť), jinak    */
/* stojí obsluha VŠECH spojení. Typicky jen vloží práci do fronty.     */
/* Smí volat [TS] funkce reactoru (např. hned odpovědět BUSY).          */
/* ------------------------------------------------------------------ */

struct kivfs_reactor_callbacks {
    /* Nové spojení. Nepovinné – může být NULL. */
    void (*on_accept)(void *user, kivfs_conn_id id, const char *peer_addr);

    /* Dorazil celý paket se zvalidovanou hlavičkou. Callback PŘEBÍRÁ
     * vlastnictví *pkt (zkopíruje si strukturu, např. do fronty);
     * reactor ho pak už nepoužije ani neuvolní. */
    void (*on_packet)(void *user, kivfs_conn_id id, struct kivfs_packet *pkt);

    /* Spojení zaniklo. Po návratu je id neplatné. Rozpracovaný požadavek
     * na tomto spojení už nemá komu odpovědět (kivfs_reactor_send ho zahodí).
     * err = errno příčiny, nebo 0. */
    void (*on_close)(void *user, kivfs_conn_id id, enum kivfs_close_reason reason, int err);

    /* Libovolný kontext serveru; předává se do všech callbacků. */
    void *user;
};

struct kivfs_reactor_config {
    const char *bind_addr;     /* NULL = všechna rozhraní */
    uint16_t    port;
    int         backlog;       /* fronta spojení čekajících na accept */
    uint32_t    max_conns;     /* nad limit se nové spojení hned zavře; zároveň
                                * chrání proti vyčerpání file deskriptorů */
    int         rx_timeout_ms; /* max. doba od 1. bajtu do posledního bajtu
                                * paketu – ochrana proti "slowloris" (protistrana
                                * posílá po kapkách a drží zdroje);
                                * 0 = bez limitu. Nečinné spojení MEZI pakety
                                * se nezavírá (spojení mezi uzly žijí dlouho). */
};

/* ------------------------------------------------------------------ */
/* Životní cyklus                                                      */
/* ------------------------------------------------------------------ */

/* Neprůhledná struktura: vnitřek (epoll fd, eventfd, tabulka spojení,
 * mailbox) zná jen reactor.c. Zvenku se s ní pracuje jen přes ukazatel. */
struct kivfs_reactor;

/* Vytvoří listen socket, epoll a eventfd. Konfigurace i callbacky se
 * zkopírují (volající je nemusí držet naživu). */
int  kivfs_reactor_create(struct kivfs_reactor **out,
                          const struct kivfs_reactor_config *cfg,
                          const struct kivfs_reactor_callbacks *cb);

/* Smyčka událostí. Blokuje volající vlákno (to se tím stává I/O vláknem),
 * dokud někdo nezavolá kivfs_reactor_stop. Před návratem zavře všechna
 * spojení (on_close s KIVFS_CLOSE_SHUTDOWN). */
int  kivfs_reactor_run(struct kivfs_reactor *r);

/* [TS] Požádá smyčku o ukončení; vrací hned, na konec smyčky nečeká. */
void kivfs_reactor_stop(struct kivfs_reactor *r);

/* Uvolní reactor včetně nevyřízených paketů v mailboxu.
 * Volat až po návratu run A po ukončení všech vláken, která by ještě mohla
 * volat [TS] funkce – jinak use-after-free.
 * Pořadí při vypínání: stop -> počkat na run -> zastavit workery -> destroy. */
void kivfs_reactor_destroy(struct kivfs_reactor *r);

/* ------------------------------------------------------------------ */
/* Volání z workerů                                                    */
/* ------------------------------------------------------------------ */

/* [TS] Zařadí paket k odeslání na spojení id.
 * Reactor paket VŽDY převezme (i při chybě) a *pkt vyprázdní – volající
 * ho už nesmí použít ani uvolnit. Samotný zápis proběhne později v I/O
 * vlákně (při plném TCP bufferu postupně přes EPOLLOUT).
 * KIVFS_OK znamená "přijato k odeslání", NE "doručeno". Pokud spojení
 * mezitím zaniklo, paket se tiše zahodí – o zániku informuje on_close.
 * Paket s KIVFS_RESPONSE_FLAG zároveň obnoví čtení ze spojení.
 * Chyby: KIVFS_ENOMEM, KIVFS_EIO (reactor už neběží). */
int  kivfs_reactor_send(struct kivfs_reactor *r, kivfs_conn_id id, struct kivfs_packet *pkt);

/* [TS] Zavře spojení, jakmile se odešlou pakety, které na něj už byly
 * zařazené. Neexistující (nebo už zaniklé) id se ignoruje.
 * Následuje on_close s KIVFS_CLOSE_LOCAL. */
void kivfs_reactor_close_conn(struct kivfs_reactor *r, kivfs_conn_id id);

#endif /* KIVFS_REACTOR_H */
