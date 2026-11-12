#ifndef KIVFS_SOCKET_H
#define KIVFS_SOCKET_H

/*
 * KIVFS – nízkoúrovňová práce s TCP sockety
 * =========================================
 *
 * Tenká vrstva nad POSIX sockety, která:
 *   - sjednotí ošetření chyb (errno -> kódy z errors.h),
 *   - schová opakující se detaily (EINTR, částečné čtení/zápis, nastavení),
 *   - neví nic o paketech – přenáší jen bajty.
 *
 * Používají ji dva odlišné světy:
 *   BLOKUJÍCÍ (channel.h, odchozí spojení): volání čeká, dokud nepřenese vše
 *     nebo nevyprší timeout. Jednoduchý lineární kód.
 *   NEBLOKUJÍCÍ (epoll reactor, příchozí spojení): volání nikdy nečeká;
 *     když teď nejde nic přenést, vrátí KIVFS_EAGAIN a reactor to zkusí,
 *     až epoll ohlásí, že socket je připravený.
 *
 * Pojmy, na které se komentáře odkazují:
 *   EINTR  – systémové volání přerušil signál, aniž by cokoli přeneslo.
 *            Není to chyba, volání se prostě zopakuje. Řeší se tu interně.
 *   Částečný přenos – read/write může přenést MÉNĚ bajtů, než jsme chtěli
 *            (TCP je proud bajtů, ne zpráv). *_full funkce proto běží ve
 *            smyčce, dokud nepřenesou vše.
 *
 * Všechny funkce vracející int vrací kód z errors.h, nebo fd (>= 0) tam,
 * kde je to uvedeno. errno z posledního systémového volání zůstává
 * zachované, takže ho volající může zalogovat (strerror(errno)).
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>   /* struct iovec */

/* Dost místa pro "ip:port" včetně nejdelší IPv6 adresy (46 znaků) + port. */
#define KIVFS_SOCK_ADDR_STRLEN  64

/* Max. počet bufferů pro kivfs_sock_writev_full. Paket potřebuje 2
 * (hlavička + payload); funkce si iovec kopíruje na zásobník, aby mohla
 * posouvat ukazatele při částečném zápisu, proto pevný strop. */
#define KIVFS_SOCK_MAX_IOV      8

/* Volat JEDNOU při startu procesu (klient i server).
 * Výchozí chování Unixu: zápis do spojení, které protistrana zavřela, pošle
 * procesu signál SIGPIPE a ten ho UKONČÍ. Po ignorování SIGPIPE zápis jen
 * vrátí chybu EPIPE, kterou ošetříme běžně (-> KIVFS_EIO). */
int  kivfs_sock_ignore_sigpipe(void);

/* ------------------------------------------------------------------ */
/* Navázání spojení                                                    */
/* ------------------------------------------------------------------ */

/* Vytvoří naslouchající socket na bind_addr:port pro epoll reactor.
 *   - bind_addr NULL = všechna rozhraní (typické v Dockeru)
 *   - backlog = délka fronty spojení čekajících na accept
 *   - nastaví SO_REUSEADDR: po restartu serveru jde port hned znovu použít
 *     (jinak by bind ještě ~minutu hlásil "Address already in use",
 *     dokud staré spojení visí ve stavu TIME_WAIT)
 *   - socket je NEBLOKUJÍCÍ
 * Vrací fd (>= 0) nebo chybu. */
int  kivfs_sock_listen(const char *bind_addr, uint16_t port, int backlog);

/* Přijme JEDNO čekající spojení z neblokujícího listen fd.
 * Reactor ji po ohlášení připravenosti volá ve smyčce, dokud nevrátí
 * KIVFS_EAGAIN – najednou může čekat víc spojení.
 *   - nový fd je NEBLOKUJÍCÍ a má TCP_NODELAY
 *   - peer_addr (smí být NULL) dostane "ip:port" protistrany pro logy
 * Vrací fd (>= 0), KIVFS_EAGAIN (už nic nečeká) nebo chybu. */
int  kivfs_sock_accept(int listen_fd, char *peer_addr, size_t peer_addr_size);

/* Odchozí spojení na host:port (host = jméno i IP; IPv4 i IPv6).
 * Běžný blokující connect na nedostupnou adresu visí i desítky sekund,
 * proto se interně dělá neblokující connect + poll() s timeout_ms
 * (timeout_ms <= 0 = bez limitu). Zkouší postupně všechny adresy,
 * které vrátí getaddrinfo.
 *   - výsledný fd je BLOKUJÍCÍ (pro channel) a má TCP_NODELAY
 * Vrací fd (>= 0), KIVFS_ERESOLVE, KIVFS_ETIMEOUT nebo KIVFS_EIO. */
int  kivfs_sock_connect(const char *host, uint16_t port, int timeout_ms);

/* Zavře *fd a nastaví *fd = -1; pro *fd < 0 nic nedělá.
 * Díky nastavení na -1 je bezpečné volat ji opakovaně (žádný dvojitý close,
 * který by mohl zavřít cizí, mezitím znovu přidělený fd). */
void kivfs_sock_close(int *fd);

/* ------------------------------------------------------------------ */
/* Nastavení                                                           */
/* ------------------------------------------------------------------ */

/* Přepne fd do neblokujícího režimu (O_NONBLOCK). */
int  kivfs_sock_set_nonblocking(int fd);

/* Vypne Nagleův algoritmus (TCP_NODELAY). Nagle zdržuje malé zápisy
 * a čeká, jestli nepřibudou další data; v kombinaci se zpožděným ACK
 * protistrany to u request-response protokolu přidá až desítky ms
 * na KAŽDÝ požadavek. My posíláme celé pakety najednou, takže ho nechceme. */
int  kivfs_sock_set_nodelay(int fd);

/* Nastaví SO_RCVTIMEO + SO_SNDTIMEO pro blokující fd.
 * Pozor: timeout platí na JEDNO systémové volání, ne na celý paket.
 * Protistrana, která posílá po jednom bajtu těsně pod limitem, může
 * čtení paketu natahovat. timeout_ms <= 0 = bez limitu. */
int  kivfs_sock_set_timeout(int fd, int timeout_ms);

/* Zapíše "ip:port" protistrany do dst (pro logy). */
int  kivfs_sock_peer_str(int fd, char *dst, size_t dst_size);

/* ------------------------------------------------------------------ */
/* Blokující I/O (pro channel) – přenese VŠE, nebo vrátí chybu          */
/* ------------------------------------------------------------------ */

/* Přečte přesně len bajtů. Návratové hodnoty rozlišují DRUH konce spojení:
 *   KIVFS_OK        přečteno všech len bajtů
 *   KIVFS_ECLOSED   protistrana zavřela spojení dřív, než přišel 1. bajt
 *                   (čistý konec mezi pakety)
 *   KIVFS_ETRUNC    spojení skončilo, když už část dat přišla
 *                   (pád uprostřed paketu)
 *   KIVFS_ETIMEOUT  vypršel SO_RCVTIMEO
 *   KIVFS_EIO       jiná chyba (errno) */
int  kivfs_sock_read_full(int fd, void *buf, size_t len);

/* Zapíše všechny bajty z iovcnt (1..KIVFS_SOCK_MAX_IOV) bufferů (scatter-gather zápis).
 * Hlavička a payload paketu leží ve dvou různých bufferech; writev je
 * odešle jedním systémovým voláním bez kopírování do společného bufferu
 * a typicky v jednom TCP segmentu.
 * Vrací KIVFS_OK, KIVFS_ETIMEOUT nebo KIVFS_EIO. */
int  kivfs_sock_writev_full(int fd, const struct iovec *iov, int iovcnt);

/* ------------------------------------------------------------------ */
/* Neblokující I/O (pro reactor) – přenese, kolik právě jde             */
/* ------------------------------------------------------------------ */

/* Jedno read/write bez čekání. Typ long, protože vrací i počet bajtů.
 *   > 0             počet přenesených bajtů (může být méně než len!)
 *   KIVFS_EAGAIN    teď nic přenést nejde, počkej na epoll
 *   KIVFS_ECLOSED   (jen read) protistrana zavřela spojení
 *   KIVFS_EIO       jiná chyba (errno)
 * Kde v paketu se právě nacházíme, si pamatuje reactor, ne tato funkce. */
long kivfs_sock_read_some (int fd, void *buf, size_t len);
long kivfs_sock_write_some(int fd, const void *buf, size_t len);

#endif /* KIVFS_SOCKET_H */
