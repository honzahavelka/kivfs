#ifndef KIVFS_REACTOR_H
#define KIVFS_REACTOR_H

/*
 * KIVFS – epoll reactor pro příchozí I ODCHOZÍ spojení (jen Linux)
 * ================================================================
 *
 * Varianta C: modul nemá žádné blokující síťové volání. Všechny sockety
 * (příchozí, odchozí, listen) vlastní jedno I/O vlákno a vše, co se na nich
 * stane, oznamuje callbackem. Modul si callbacky typicky jen převede na
 * události ve své frontě a zpracuje je ve workeru.
 *
 * Tok zpráv – PŘÍCHOZÍ spojení (protistrana se ptá, my odpovídáme):
 *
 *   nové spojení -> accept -> přidělí conn_id -> on_accept()
 *   bajty        -> čtení po kouscích -> celý paket -> on_packet() -> (fronta -> worker)
 *   worker       -> kivfs_reactor_send() -> mailbox + eventfd -> I/O vlákno zapíše
 *   konec        -> on_close(důvod)
 *
 * Tok zpráv – ODCHOZÍ spojení (my se ptáme, protistrana odpovídá):
 *
 *   worker       -> kivfs_reactor_connect() -> conn_id hned -> neblokující connect
 *                -> on_connect()                     (nebo on_close(CONNECT))
 *   worker       -> kivfs_reactor_request(tag) -> odchozí fronta spojení
 *                -> I/O vlákno odešle, až je spojení volné
 *   odpověď / timeout / pád spojení -> on_response(tag, err) – VŽDY právě jednou
 *
 * Časovače: kivfs_reactor_timer(tag) -> po uplynutí on_timer(tag). Modul
 * nesmí ve workeru spát (backoff při reconnectu, timeout kola Sync…),
 * proto čas měří reactor.
 *
 * Celý reactor běží v JEDNOM I/O vlákně (kivfs_reactor_run). Jen toto vlákno
 * sahá na sockety a na tabulku spojení, takže ty nepotřebují zámky. Ostatní
 * vlákna smí volat pouze funkce označené [TS] (thread-safe); ty jen vloží
 * požadavek do mailboxu (chráněného mutexem) a probudí I/O vlákno přes
 * eventfd, který je zaregistrovaný v epollu jako další "socket".
 *
 * Reactor neví nic o frontě, workerech, HELLO ani Lamportových hodinách –
 * celé pakety předává callbackem. Pole header.timestamp odchozích
 * požadavků nastavuje volající; reactor ho jen použije k párování odpovědi.
 *
 * Směr spojení je pevný (pravidlo "kdo spojení založil, ten se ptá"):
 *   - příchozí: chodí jen požadavky, odchází jen odpovědi (kivfs_reactor_send)
 *   - odchozí:  odchází jen požadavky (kivfs_reactor_request), chodí jen
 *               odpovědi. Požadavek od protistrany na odchozím spojení =
 *               porušení protokolu -> spojení se zavře (KIVFS_CLOSE_PROTOCOL).
 *
 * Pravidlo "jeden požadavek na spojení" (platí v obou směrech):
 *   Příchozí: po doručení paketu (on_packet) reactor z daného spojení
 *   PŘESTANE ČÍST, dokud na něj někdo nepošle odpověď (kivfs_reactor_send
 *   s paketem, který má KIVFS_RESPONSE_FLAG). Důsledky:
 *     - zprávy jednoho spojení se zpracují v pořadí (FIFO, nutné pro Lamporta)
 *     - na jednom spojení nikdy nepracují dva workery najednou
 *     - ve frontě workerů je nejvýš tolik paketů, kolik je spojení
 *     - ve frontě k odeslání má každé spojení nejvýš jednu odpověď
 *   Pokud worker kvůli chybě nikdy neodpoví, spojení zamrzne – protistrana
 *   ho ukončí svým timeoutem.
 *   Odchozí: na spojení je nejvýš jeden požadavek "na drátě". Další čekají
 *   v odchozí frontě spojení (max_pending) a odešlou se, až přijde odpověď.
 *   Na pořadí odpovědí RŮZNÝCH spojení se spoléhat nesmí.
 *
 * Zápis: hlavička a payload paketu se zapisují jedním writev() (jeden TCP
 * segment u malých paketů místo dvou).
 *
 * epoll běží v level-triggered režimu: epoll hlásí socket, dokud v něm
 * jsou data. Je to jednodušší a odolnější než edge-triggered, kde by se
 * muselo vždy číst až do EAGAIN, jinak se na zbylá data "zapomene".
 *
 * Mapování conn_id -> kdo je na spojení (z HELLO) drží vrstva nad
 * reactorem, ne reactor: naplní ho při zpracování HELLO a smaže v on_close.
 *
 * Návratové kódy: errors.h.
 */

#include <stdint.h>
#include <sys/socket.h>
#include "proto/packet.h"

/* ------------------------------------------------------------------ */
/* Identifikace spojení                                                */
/* ------------------------------------------------------------------ */

/* Spojení se mimo I/O vlákno identifikuje číslem, NE ukazatelem.
 * Horních 32 bitů = generace, dolních 32 bitů = index slotu v tabulce.
 * Když spojení zanikne a jeho slot se použije pro nové, generace se zvýší.
 * Pozdní odpověď workera na staré conn_id se tak pozná a zahodí – neodejde
 * cizímu klientovi a nesáhne do uvolněné paměti. */
typedef uint64_t kivfs_conn_id;

/* Záruka pro vrstvy nad reactorem: index slotu (dolních 32 bitů) je vždy
 * < max_conns + max_out_conns z konfigurace. Smí si tedy vést vlastní pole
 * indexované slotem (např. kdo je na spojení) a platnost položky ověřit
 * porovnáním celého conn_id.
 *
 * Generace začínají od 1, takže 0 nikdy není platné conn_id. */
#define KIVFS_CONN_ID_INVALID 0u

/* Proč spojení skončilo – pro logy a pro vrstvu nad reactorem
 * (např. detekce pádu protistrany). */
enum kivfs_close_reason {
    KIVFS_CLOSE_PEER,         /* protistrana zavřela čistě mezi pakety */
    KIVFS_CLOSE_IO_ERROR,     /* chyba socketu nebo konec uprostřed paketu */
    KIVFS_CLOSE_PROTOCOL,     /* špatný magic, data_size > KIVFS_MAX_PAYLOAD,
                               * paket ve špatném směru nebo odpověď, která
                               * nepatří k rozpracovanému požadavku */
    KIVFS_CLOSE_TIMEOUT,      /* paket nedorazil celý do rx_timeout_ms */
    KIVFS_CLOSE_LOCAL,        /* zavřeno přes kivfs_reactor_close_conn */
    KIVFS_CLOSE_SHUTDOWN,     /* reactor se ukončuje */
    KIVFS_CLOSE_CONNECT,      /* odchozí: připojení selhalo (odmítnuto,
                               * nedostupné, connect_timeout_ms); on_connect
                               * nebyl zavolán */
    KIVFS_CLOSE_RESP_TIMEOUT  /* odchozí: na požadavek "na drátě" nepřišla
                               * odpověď včas – protistrana visí nebo je
                               * přetížená, spojení už není použitelné */
};

/* ------------------------------------------------------------------ */
/* Callbacky – volají se VŽDY v I/O vlákně                             */
/* Musí být rychlé a nesmí blokovat (žádná DB, disk ani síť), jinak    */
/* stojí obsluha VŠECH spojení. Typicky jen vloží práci do fronty.     */
/* Smí volat [TS] funkce reactoru (např. hned odpovědět BUSY).          */
/* ------------------------------------------------------------------ */

struct kivfs_reactor_callbacks {
    /* Nové PŘÍCHOZÍ spojení. Nepovinné – může být NULL. */
    void (*on_accept)(void *user, kivfs_conn_id id, const char *peer_addr);

    /* ODCHOZÍ spojení je navázané. Nepovinné – může být NULL (požadavky
     * zařazené před navázáním se odešlou samy). */
    void (*on_connect)(void *user, kivfs_conn_id id);

    /* Na PŘÍCHOZÍM spojení dorazil celý požadavek se zvalidovanou
     * hlavičkou. Callback PŘEBÍRÁ vlastnictví *pkt (zkopíruje si
     * strukturu, např. do fronty); reactor ho pak už nepoužije ani neuvolní. */
    void (*on_packet)(void *user, kivfs_conn_id id, struct kivfs_packet *pkt);

    /* Výsledek požadavku z kivfs_reactor_request – pro každý přijatý
     * požadavek PRÁVĚ JEDNOU (tag = hodnota zadaná volajícím).
     *   err == KIVFS_OK:   *resp je odpověď (stav na drátě je v
     *                      resp->header.return_value); callback PŘEBÍRÁ
     *                      vlastnictví *resp jako u on_packet.
     *   jinak resp == NULL a err je:
     *     KIVFS_ETIMEOUT   vypršel timeout_ms
     *     KIVFS_ECONNLOST  spojení zaniklo dřív, než přišla odpověď
     *     KIVFS_EAGAIN     odchozí fronta spojení je plná (max_pending)
     *     KIVFS_EINVAL     id není odchozí spojení
     *     KIVFS_ESHUTDOWN  reactor se ukončuje
     *   POZOR: ETIMEOUT i ECONNLOST znamenají "výsledek NEZNÁMÝ", ne
     *   "neprovedeno" – požadavek mohl být odeslán a protistrana ho mohla
     *   provést. Volající musí počítat s opakováním (idempotence) nebo se
     *   na výsledek doptat. */
    void (*on_response)(void *user, kivfs_conn_id id, uint64_t tag, int err,
                        struct kivfs_packet *resp);

    /* Uplynul časovač z kivfs_reactor_timer. Nepovinné, pokud modul
     * časovače nepoužívá. */
    void (*on_timer)(void *user, uint64_t tag);

    /* Spojení (příchozí i odchozí) zaniklo. Každé conn_id skončí PRÁVĚ
     * JEDNÍM on_close – i odchozí spojení, které se nikdy nenavázalo
     * (KIVFS_CLOSE_CONNECT). Po návratu je id neplatné.
     * Příchozí: rozpracovaný požadavek už nemá komu odpovědět
     * (kivfs_reactor_send ho zahodí).
     * Odchozí: všechny požadavky, které reactor na spojení přijal, už
     * dostaly on_response (KIVFS_ECONNLOST) – on_close je poslední událost
     * spojení. Výjimka: požadavek zařazený na už mrtvé id (worker o zániku
     * ještě neví) dostane KIVFS_ECONNLOST až po on_close.
     * outgoing = 1 u odchozího spojení (kivfs_reactor_connect), 0 u příchozího.
     * err = errno příčiny, nebo 0. */
    void (*on_close)(void *user, kivfs_conn_id id, int outgoing,
                     enum kivfs_close_reason reason, int err);

    /* Libovolný kontext modulu; předává se do všech callbacků. */
    void *user;
};

/* Hodnota 0 u číselných položek = výchozí hodnota (uvedena v komentáři). */
struct kivfs_reactor_config {
    const char *bind_addr;     /* NULL = všechna rozhraní */
    uint16_t    port;
    int         backlog;       /* fronta spojení čekajících na accept */
    uint32_t    max_conns;     /* max. PŘÍCHOZÍCH spojení; nad limit se nové
                                * spojení hned zavře; chrání i proti vyčerpání
                                * file deskriptorů */
    uint32_t    max_out_conns; /* max. ODCHOZÍCH spojení (výchozí 64). Oddělený
                                * limit: záplava příchozích spojení nesmí
                                * modulu znemožnit připojit se k závislostem. */
    uint32_t    max_pending;   /* max. požadavků na jedno odchozí spojení včetně
                                * toho "na drátě" (výchozí 16); další dostanou
                                * on_response(KIVFS_EAGAIN) */
    int         rx_timeout_ms; /* max. doba od 1. bajtu do posledního bajtu
                                * paketu – ochrana proti "slowloris" (protistrana
                                * posílá po kapkách a drží zdroje);
                                * 0 = bez limitu. Nečinné spojení MEZI pakety
                                * se nezavírá (spojení mezi moduly žijí dlouho). */
    int         connect_timeout_ms; /* max. doba neblokujícího connect
                                * (výchozí 5000) */
};

/* ------------------------------------------------------------------ */
/* Životní cyklus                                                      */
/* ------------------------------------------------------------------ */

/* Neprůhledná struktura: vnitřek (epoll fd, eventfd, tabulka spojení,
 * mailbox, časovače) zná jen reactor.c. Zvenku jen přes ukazatel. */
struct kivfs_reactor;

/* Vytvoří listen socket, epoll a eventfd. Konfigurace i callbacky se
 * zkopírují (volající je nemusí držet naživu).
 * KIVFS_EINVAL, pokud chybí callback potřebný pro zvolené použití
 * (on_packet a on_close vždy; on_response se kontroluje až při request). */
int  kivfs_reactor_create(struct kivfs_reactor **out,
                          const struct kivfs_reactor_config *cfg,
                          const struct kivfs_reactor_callbacks *cb);

/* Smyčka událostí. Blokuje volající vlákno (to se tím stává I/O vláknem),
 * dokud někdo nezavolá kivfs_reactor_stop. Před návratem:
 *   - všem nevyřízeným požadavkům zavolá on_response(KIVFS_ESHUTDOWN),
 *   - zavře všechna spojení (on_close s KIVFS_CLOSE_SHUTDOWN),
 *   - nevypršené časovače zahodí BEZ callbacku.
 * Invariant "každý požadavek právě jeden on_response" tak platí i při
 * vypínání; fronta modulu proto musí běžet, dokud run nevrátí. */
int  kivfs_reactor_run(struct kivfs_reactor *r);

/* [TS] Požádá smyčku o ukončení; vrací hned, na konec smyčky nečeká. */
void kivfs_reactor_stop(struct kivfs_reactor *r);

/* Uvolní reactor včetně nevyřízených paketů v mailboxu.
 * Volat až po návratu run A po ukončení všech vláken, která by ještě mohla
 * volat [TS] funkce – jinak use-after-free.
 * Pořadí při vypínání: stop -> počkat na run -> zastavit workery -> destroy. */
void kivfs_reactor_destroy(struct kivfs_reactor *r);

/* ------------------------------------------------------------------ */
/* Příchozí spojení – odpovídání                                       */
/* ------------------------------------------------------------------ */

/* [TS] Zařadí ODPOVĚĎ k odeslání na PŘÍCHOZÍ spojení id.
 * Reactor paket VŽDY převezme (i při chybě) a *pkt vyprázdní – volající
 * ho už nesmí použít ani uvolnit. Samotný zápis proběhne později v I/O
 * vlákně (při plném TCP bufferu postupně přes EPOLLOUT).
 * KIVFS_OK znamená "přijato k odeslání", NE "doručeno". Pokud spojení
 * mezitím zaniklo, nebo jde o odchozí spojení, paket se zahodí
 * (o zániku informuje on_close).
 * Paket s KIVFS_RESPONSE_FLAG zároveň obnoví čtení ze spojení.
 * Chyby: KIVFS_ENOMEM, KIVFS_EIO (reactor už neběží). */
int  kivfs_reactor_send(struct kivfs_reactor *r, kivfs_conn_id id, struct kivfs_packet *pkt);

/* ------------------------------------------------------------------ */
/* Odchozí spojení – ptaní se                                          */
/* ------------------------------------------------------------------ */

/* [TS] Založí ODCHOZÍ spojení na již přeloženou adresu (getaddrinfo volá
 * volající mimo I/O vlákno – blokuje). conn_id vrátí HNED v *out, ještě
 * před navázáním; požadavky na něj lze zařadit okamžitě (např. HELLO)
 * a odešlou se po navázání v pořadí zařazení.
 * Výsledek: on_connect, nebo on_close(KIVFS_CLOSE_CONNECT) – ten pak
 * přijde až po on_response(KIVFS_ECONNLOST) všech zařazených požadavků.
 * Reactor sám znovu NEpřipojuje – backoff a reconnect řeší vrstva nad ním
 * (pomocí kivfs_reactor_timer).
 * Chyby (pak se nic nezaloží a žádný callback nepřijde):
 *   KIVFS_EINVAL (addrlen), KIVFS_EAGAIN (vyčerpáno max_out_conns),
 *   KIVFS_ENOMEM, KIVFS_EIO (reactor už neběží). */
int  kivfs_reactor_connect(struct kivfs_reactor *r,
                           const struct sockaddr *addr, socklen_t addrlen,
                           kivfs_conn_id *out);

/* [TS] Zařadí POŽADAVEK na ODCHOZÍ spojení id.
 * Reactor paket VŽDY převezme (jako kivfs_reactor_send). Pole
 * header.timestamp nastavuje volající; odpověď se páruje podle ref ==
 * header.timestamp požadavku a kódu (request_code | KIVFS_RESPONSE_FLAG).
 * Nesedí-li, spojení se zavře (KIVFS_CLOSE_PROTOCOL).
 *
 * timeout_ms (> 0) běží od zavolání, tj. zahrnuje i čekání v odchozí
 * frontě. Vyprší-li:
 *   - u požadavku, který ještě čeká ve frontě: jen se vyřadí, spojení žije;
 *   - u požadavku "na drátě": spojení se zavře (KIVFS_CLOSE_RESP_TIMEOUT),
 *     protože protistrana kvůli pravidlu "jeden požadavek na spojení"
 *     nepřečte nic dalšího, dokud neodpoví. Ostatní požadavky spojení
 *     dostanou KIVFS_ECONNLOST.
 *
 * Vrací KIVFS_OK -> přijde PRÁVĚ JEDEN on_response s tímto tagem
 * (i když spojení mezitím zaniklo nebo id nikdy neplatilo).
 * Jinak žádný on_response nepřijde:
 *   KIVFS_EINVAL (paket je odpověď, timeout_ms <= 0, chybí on_response),
 *   KIVFS_ENOMEM, KIVFS_EIO (reactor už neběží).
 *
 * tag je pro reactor neprůhledný; modul do něj kóduje, komu výsledek
 * patří (číslo operace, případně index workeru). */
int  kivfs_reactor_request(struct kivfs_reactor *r, kivfs_conn_id id,
                           struct kivfs_packet *req, uint64_t tag, int timeout_ms);

/* ------------------------------------------------------------------ */
/* Společné                                                            */
/* ------------------------------------------------------------------ */

/* [TS] Zavře spojení id (příchozí i odchozí). Neexistující (nebo už
 * zaniklé) id se ignoruje. Následuje on_close s KIVFS_CLOSE_LOCAL.
 * Příchozí: zavře se, až se odešlou už zařazené odpovědi.
 * Odchozí: zavře se hned; zařazené i rozpracované požadavky dostanou
 * on_response(KIVFS_ECONNLOST). */
void kivfs_reactor_close_conn(struct kivfs_reactor *r, kivfs_conn_id id);

/* [TS] Jednorázový časovač: za delay_ms (>= 0) zavolá on_timer(tag).
 * Zrušit nejde – modul si pozdní časovač pozná podle tagu (např. tag
 * obsahuje generaci operace) a ignoruje ho.
 * Přesnost: řádově jednotky ms, nikdy dřív než delay_ms.
 * Chyby: KIVFS_EINVAL (delay_ms < 0 nebo chybí on_timer),
 * KIVFS_ENOMEM, KIVFS_EIO (reactor už neběží). */
int  kivfs_reactor_timer(struct kivfs_reactor *r, int delay_ms, uint64_t tag);

#endif /* KIVFS_REACTOR_H */
