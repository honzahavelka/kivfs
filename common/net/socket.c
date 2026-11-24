/*
 * Feature-test makra MUSÍ být před prvním #include. Projekt se překládá
 * jako čisté C11, které samo nezpřístupní POSIX funkce (getaddrinfo, poll,
 * sigaction...). Na macOS by _POSIX_C_SOURCE naopak skrylo typy, které
 * potřebuje <netinet/tcp.h>, proto tam použijeme _DARWIN_C_SOURCE.
 */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include "net/socket.h"
#include "proto/errors.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * Pojistka proti SIGPIPE i bez kivfs_sock_ignore_sigpipe():
 *   Linux: příznak MSG_NOSIGNAL u send/sendmsg (platí pro jedno volání)
 *   macOS: MSG_NOSIGNAL nemá, místo toho volba socketu SO_NOSIGPIPE
 */
#ifdef MSG_NOSIGNAL
#  define SEND_FLAGS MSG_NOSIGNAL
#else
#  define SEND_FLAGS 0
#endif

/* ================================================================== */
/* Interní pomocníci                                                   */
/* ================================================================== */

/* Monotónní čas v ms – pro deadline u connect. CLOCK_MONOTONIC neskáče
 * při změně systémového času (NTP), na rozdíl od time(). */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* close() se zachováním errno – aby úklid po chybě nepřepsal příčinu. */
static void close_keep_errno(int fd)
{
    int saved = errno;
    close(fd);
    errno = saved;
}

static void set_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

static int set_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
        return KIVFS_EIO;
    return KIVFS_OK;
}

/* "ip:port", IPv6 v hranatých závorkách: "[::1]:9000". */
static void addr_to_str(const struct sockaddr *sa, char *dst, size_t dst_size)
{
    char ip[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)(const void *)sa;
        inet_ntop(AF_INET, &in4->sin_addr, ip, sizeof ip);
        port = ntohs(in4->sin_port);
        snprintf(dst, dst_size, "%s:%u", ip, port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)(const void *)sa;
        inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof ip);
        port = ntohs(in6->sin6_port);
        snprintf(dst, dst_size, "[%s]:%u", ip, port);
    } else {
        snprintf(dst, dst_size, "?");
    }
}

static int resolve(const char *host, uint16_t port, int passive, struct addrinfo **res)
{
    struct addrinfo hints;
    char port_str[8];

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;      /* IPv4 i IPv6 */
    hints.ai_socktype = SOCK_STREAM;    /* TCP */
    hints.ai_flags    = passive ? AI_PASSIVE : 0;
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);

    return getaddrinfo(host, port_str, &hints, res) == 0 ? KIVFS_OK : KIVFS_ERESOLVE;
}

/* ================================================================== */
/* Navázání spojení                                                    */
/* ================================================================== */

int kivfs_sock_ignore_sigpipe(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGPIPE, &sa, NULL) == 0 ? KIVFS_OK : KIVFS_EIO;
}

int kivfs_sock_listen(const char *bind_addr, uint16_t port, int backlog)
{
    struct addrinfo *res;
    int rc = resolve(bind_addr, port, 1, &res);
    if (rc != KIVFS_OK)
        return rc;

    /* getaddrinfo může vrátit víc adres (např. IPv4 i IPv6); použijeme
     * první, na kterou jde bind. V Dockeru bez IPv6 tak IPv6 tiše selže
     * a vezme se IPv4. */
    rc = KIVFS_EIO;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0
            && bind(fd, ai->ai_addr, ai->ai_addrlen) == 0
            && listen(fd, backlog) == 0
            && kivfs_sock_set_nonblocking(fd) == KIVFS_OK) {
            rc = fd;
            break;
        }
        close_keep_errno(fd);
    }

    freeaddrinfo(res);
    return rc;
}

int kivfs_sock_accept(int listen_fd, char *peer_addr, size_t peer_addr_size)
{
    struct sockaddr_storage ss;
    socklen_t ss_len;
    int fd;

    for (;;) {
        ss_len = sizeof ss;
        fd = accept(listen_fd, (struct sockaddr *)&ss, &ss_len);
        if (fd >= 0)
            break;
        /* EINTR: přerušeno signálem. ECONNABORTED: klient spojení zrušil
         * dřív, než jsme ho přijali – v obou případech zkusit další. */
        if (errno == EINTR || errno == ECONNABORTED)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return KIVFS_EAGAIN;
        return KIVFS_EIO;   /* např. EMFILE – došly file deskriptory */
    }

    /* Linux nové spojení neblokující NEDĚDÍ (macOS ano) – nastavit vždy. */
    if (kivfs_sock_set_nonblocking(fd) != KIVFS_OK
        || kivfs_sock_set_nodelay(fd) != KIVFS_OK) {
        close_keep_errno(fd);
        return KIVFS_EIO;
    }
    set_nosigpipe(fd);

    if (peer_addr != NULL && peer_addr_size > 0)
        addr_to_str((struct sockaddr *)&ss, peer_addr, peer_addr_size);
    return fd;
}

/* Jeden pokus o connect s deadlinem. fd je po návratu pořád neblokující. */
static int connect_one(int fd, const struct addrinfo *ai, long long deadline)
{
    if (kivfs_sock_set_nonblocking(fd) != KIVFS_OK)
        return KIVFS_EIO;

    /* Neblokující connect hned vrátí EINPROGRESS a spojení se navazuje
     * na pozadí. (EINTR znamená totéž – navazování pokračuje.) */
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        return KIVFS_OK;
    if (errno != EINPROGRESS && errno != EINTR)
        return KIVFS_EIO;

    /* Počkáme, až bude socket zapisovatelný = navazování skončilo
     * (úspěchem i neúspěchem). */
    struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
    for (;;) {
        int wait_ms = -1;                       /* -1 = bez limitu */
        if (deadline >= 0) {
            long long left = deadline - now_ms();
            if (left <= 0) {
                errno = ETIMEDOUT;
                return KIVFS_ETIMEOUT;
            }
            wait_ms = (int)left;
        }
        int n = poll(&pfd, 1, wait_ms);
        if (n > 0)
            break;
        if (n == 0) {
            errno = ETIMEDOUT;
            return KIVFS_ETIMEOUT;
        }
        if (errno != EINTR)                     /* EINTR -> znovu se zbytkem času */
            return KIVFS_EIO;
    }

    /* Jak navazování dopadlo, prozradí SO_ERROR (0 = úspěch). */
    int err = 0;
    socklen_t err_len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0)
        return KIVFS_EIO;
    if (err != 0) {
        errno = err;                            /* např. ECONNREFUSED */
        return err == ETIMEDOUT ? KIVFS_ETIMEOUT : KIVFS_EIO;
    }
    return KIVFS_OK;
}

int kivfs_sock_connect(const char *host, uint16_t port, int timeout_ms)
{
    struct addrinfo *res;
    int rc = resolve(host, port, 0, &res);
    if (rc != KIVFS_OK)
        return rc;

    /* Jeden společný deadline pro všechny adresy: timeout_ms je limit
     * na celé kivfs_sock_connect, ne na každý pokus zvlášť. */
    long long deadline = timeout_ms > 0 ? now_ms() + timeout_ms : -1;

    rc = KIVFS_EIO;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        rc = connect_one(fd, ai, deadline);
        if (rc == KIVFS_OK) {
            if (set_blocking(fd) == KIVFS_OK && kivfs_sock_set_nodelay(fd) == KIVFS_OK) {
                set_nosigpipe(fd);
                freeaddrinfo(res);
                return fd;
            }
            rc = KIVFS_EIO;
        }
        close_keep_errno(fd);
        if (rc == KIVFS_ETIMEOUT)               /* čas vypršel, další adresy nezkoušet */
            break;
    }

    freeaddrinfo(res);
    return rc;
}

int kivfs_sock_resolve(const char *host, uint16_t port,
                       struct sockaddr_storage *out, socklen_t *out_len)
{
    struct addrinfo *res;
    int rc = resolve(host, port, 0, &res);
    if (rc != KIVFS_OK)
        return rc;
    if (res == NULL || res->ai_addrlen > sizeof *out) {
        if (res != NULL)
            freeaddrinfo(res);
        return KIVFS_ERESOLVE;
    }
    memset(out, 0, sizeof *out);
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *out_len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return KIVFS_OK;
}

int kivfs_sock_connect_start(const struct sockaddr *addr, socklen_t addrlen)
{
    int fd = socket(addr->sa_family, SOCK_STREAM, 0);
    if (fd < 0)
        return KIVFS_EIO;
    if (kivfs_sock_set_nonblocking(fd) != KIVFS_OK
        || kivfs_sock_set_nodelay(fd) != KIVFS_OK) {
        close_keep_errno(fd);
        return KIVFS_EIO;
    }
    set_nosigpipe(fd);

    /* 0 = navázáno hned (bývá u loopbacku), EINPROGRESS/EINTR = navazuje
     * se na pozadí. V obou případech výsledek ověří connect_finish, až
     * bude socket zapisovatelný (navázaný socket je zapisovatelný hned). */
    if (connect(fd, addr, addrlen) == 0 || errno == EINPROGRESS || errno == EINTR)
        return fd;
    close_keep_errno(fd);
    return KIVFS_EIO;
}

int kivfs_sock_connect_finish(int fd)
{
    int err = 0;
    socklen_t err_len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0)
        return KIVFS_EIO;
    if (err != 0) {
        errno = err;
        return KIVFS_EIO;
    }
    return KIVFS_OK;
}

void kivfs_sock_close(int *fd)
{
    if (*fd >= 0) {
        /* Při EINTR se close NEopakuje: na Linuxu je fd zavřený i tak
         * a opakování by mohlo zavřít cizí, mezitím přidělený fd. */
        close(*fd);
        *fd = -1;
    }
}

/* ================================================================== */
/* Nastavení                                                           */
/* ================================================================== */

int kivfs_sock_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return KIVFS_EIO;
    return KIVFS_OK;
}

int kivfs_sock_set_nodelay(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0
           ? KIVFS_OK : KIVFS_EIO;
}

int kivfs_sock_set_timeout(int fd, int timeout_ms)
{
    struct timeval tv;                          /* {0, 0} = bez limitu */
    memset(&tv, 0, sizeof tv);
    if (timeout_ms > 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0
        || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0)
        return KIVFS_EIO;
    return KIVFS_OK;
}

int kivfs_sock_peer_str(int fd, char *dst, size_t dst_size)
{
    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &ss_len) != 0)
        return KIVFS_EIO;
    addr_to_str((struct sockaddr *)&ss, dst, dst_size);
    return KIVFS_OK;
}

/* ================================================================== */
/* Blokující I/O                                                       */
/* ================================================================== */

int kivfs_sock_read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = recv(fd, p + done, len - done, 0);
        if (n > 0) {
            done += (size_t)n;                  /* částečné čtení -> pokračuj */
            continue;
        }
        if (n == 0)                             /* EOF: protistrana zavřela */
            return done == 0 ? KIVFS_ECLOSED : KIVFS_ETRUNC;
        if (errno == EINTR)
            continue;
        /* U blokujícího socketu s SO_RCVTIMEO znamená EAGAIN vypršení timeoutu. */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return KIVFS_ETIMEOUT;
        return KIVFS_EIO;                       /* např. ECONNRESET */
    }
    return KIVFS_OK;
}

int kivfs_sock_writev_full(int fd, const struct iovec *iov_in, int iovcnt)
{
    if (iovcnt < 1 || iovcnt > KIVFS_SOCK_MAX_IOV) {
        errno = EINVAL;
        return KIVFS_EIO;
    }

    /* Lokální kopie: při částečném zápisu posouváme base/len, originál
     * volajícího zůstane nedotčený. */
    struct iovec iov_buf[KIVFS_SOCK_MAX_IOV];
    memcpy(iov_buf, iov_in, (size_t)iovcnt * sizeof *iov_in);
    struct iovec *iov = iov_buf;

    /* sendmsg místo writev, aby šlo předat SEND_FLAGS (MSG_NOSIGNAL). */
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);

    while (iovcnt > 0) {
        msg.msg_iov    = iov;
        msg.msg_iovlen = (size_t)iovcnt;   /* Linux: size_t, macOS: int; 1..8 se vejde do obou */

        ssize_t n = sendmsg(fd, &msg, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return KIVFS_ETIMEOUT;          /* SO_SNDTIMEO */
            return KIVFS_EIO;                   /* např. EPIPE, ECONNRESET */
        }

        /* Přeskočit plně odeslané buffery (i prázdné), zbytek posunout. */
        size_t sent = (size_t)n;
        while (iovcnt > 0 && sent >= iov->iov_len) {
            sent -= iov->iov_len;
            iov++;
            iovcnt--;
        }
        if (iovcnt > 0) {
            iov->iov_base = (uint8_t *)iov->iov_base + sent;
            iov->iov_len -= sent;
        }
    }
    return KIVFS_OK;
}

/* ================================================================== */
/* Neblokující I/O                                                     */
/* ================================================================== */

long kivfs_sock_read_some(int fd, void *buf, size_t len)
{
    if (len == 0)
        return 0;
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n > 0)
            return (long)n;
        if (n == 0)
            return KIVFS_ECLOSED;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return KIVFS_EAGAIN;
        return KIVFS_EIO;
    }
}

long kivfs_sock_write_some(int fd, const void *buf, size_t len)
{
    if (len == 0)
        return 0;
    for (;;) {
        ssize_t n = send(fd, buf, len, SEND_FLAGS);
        if (n >= 0)
            return (long)n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return KIVFS_EAGAIN;
        return KIVFS_EIO;
    }
}

long kivfs_sock_writev_some(int fd, const struct iovec *iov, int iovcnt)
{
    if (iovcnt < 1 || iovcnt > KIVFS_SOCK_MAX_IOV) {
        errno = EINVAL;
        return KIVFS_EIO;
    }
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov    = (struct iovec *)iov;   /* sendmsg iovec nemění */
    msg.msg_iovlen = (size_t)iovcnt;
    for (;;) {
        ssize_t n = sendmsg(fd, &msg, SEND_FLAGS);
        if (n >= 0)
            return (long)n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return KIVFS_EAGAIN;
        return KIVFS_EIO;
    }
}
