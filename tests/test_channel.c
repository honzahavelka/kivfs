/*
 * Integrační testy socket.c + channel.c přes loopback (127.0.0.1).
 *
 * Každý scénář: hlavní vlákno = klient (kivfs_chan), pomocné vlákno =
 * testovací server, který přijme JEDNO spojení a chová se podle zvoleného
 * režimu (korektní echo, vadná odpověď, pád uprostřed paketu...).
 */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "errors.h"
#include "protocol.h"
#include "packet.h"
#include "socket.h"
#include "channel.h"

/* Atomické čítače – CHECK volá klientské i serverové vlákno současně. */
static atomic_int g_checks;
static atomic_int g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

#define CHECK_RC(expr, expected)                                           \
    do {                                                                   \
        int rc_ = (expr);                                                  \
        g_checks++;                                                        \
        if (rc_ != (expected)) {                                           \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s -> %d (%s), expected %s\n",   \
                    __FILE__, __LINE__, #expr, rc_, kivfs_strerror(rc_),   \
                    #expected);                                            \
        }                                                                  \
    } while (0)

#define TIMEOUT_MS 2000
#define REQ_ECHO   42

/* ------------------------------------------------------------------ */
/* Testovací server                                                    */
/* ------------------------------------------------------------------ */

enum server_mode {
    SRV_ECHO,           /* odpovídá echem, dokud klient nezavře */
    SRV_WRONG_REF,      /* odpověď se špatným refem */
    SRV_CLOSE,          /* přečte požadavek a zavře bez odpovědi */
    SRV_HALF_PACKET,    /* pošle hlavičku + část payloadu a zavře */
    SRV_HEADER_ONLY,    /* pošle jen hlavičku (data_size > 0) a zavře */
    SRV_BAD_MAGIC,      /* pošle 20 B nesmyslů */
    SRV_SLOW,           /* přečte požadavek a 600 ms mlčí */
    SRV_NONBLOCK        /* testuje neblokující API na přijatém fd */
};

struct server_ctx {
    int listen_fd;
    enum server_mode mode;
};

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

/* Počká na příchozí spojení a přijme ho (listen fd je neblokující). */
static int server_accept(int listen_fd, char *peer, size_t peer_size)
{
    struct pollfd pfd = { .fd = listen_fd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, TIMEOUT_MS) <= 0)
        return -1;
    return kivfs_sock_accept(listen_fd, peer, peer_size);
}

static void make_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

/* Odpověď = ref + kopie všech záznamů z požadavku. */
static int build_echo(const struct kivfs_packet *req, uint64_t ts, struct kivfs_packet *resp)
{
    int rc = kivfs_pkt_init_response(resp, req, ts, KIVFS_STATUS_OK);
    struct kivfs_pkt_reader r;
    const uint8_t *rec;
    uint32_t len;

    kivfs_pkt_reader_init(&r, req);
    while (rc == KIVFS_OK && kivfs_pkt_read_bytes(&r, &rec, &len) == KIVFS_OK)
        rc = kivfs_pkt_add_bytes(resp, rec, len);
    return rc;
}

static void *server_main(void *arg)
{
    struct server_ctx *ctx = arg;
    char peer[KIVFS_SOCK_ADDR_STRLEN];
    int fd = server_accept(ctx->listen_fd, peer, sizeof peer);
    CHECK(fd >= 0);
    if (fd < 0)
        return NULL;
    CHECK(strncmp(peer, "127.0.0.1:", 10) == 0);

    if (ctx->mode == SRV_NONBLOCK) {
        /* Klient zatím nic neposlal -> neblokující čtení musí hned vrátit EAGAIN. */
        uint8_t b;
        CHECK(kivfs_sock_read_some(fd, &b, 1) == KIVFS_EAGAIN);
        CHECK(kivfs_sock_write_some(fd, "x", 1) == 1);
        kivfs_sock_close(&fd);
        return NULL;
    }

    /* Ostatní režimy pracují blokujícím kanálem nad přijatým fd. */
    make_blocking(fd);
    kivfs_sock_set_timeout(fd, TIMEOUT_MS);
    struct kivfs_chan ch;
    kivfs_chan_init(&ch);
    ch.fd = fd;

    struct kivfs_packet req, resp;
    uint8_t hdr[KIVFS_HEADER_WIRE_SIZE];
    struct kivfs_header h;

    switch (ctx->mode) {
    case SRV_ECHO: {
        uint64_t ts = 100;
        int rc;
        while ((rc = kivfs_chan_recv(&ch, &req)) == KIVFS_OK) {
            CHECK_RC(build_echo(&req, ++ts, &resp), KIVFS_OK);
            CHECK_RC(kivfs_chan_send(&ch, &resp), KIVFS_OK);
            kivfs_pkt_free(&req);
            kivfs_pkt_free(&resp);
        }
        CHECK_RC(rc, KIVFS_ECLOSED);   /* klient zavřel čistě mezi pakety */
        break;
    }
    case SRV_WRONG_REF:
        CHECK_RC(kivfs_chan_recv(&ch, &req), KIVFS_OK);
        req.header.timestamp += 1;     /* odpověď bude ukazovat na jiný požadavek */
        CHECK_RC(kivfs_pkt_init_response(&resp, &req, 1, KIVFS_STATUS_OK), KIVFS_OK);
        CHECK_RC(kivfs_chan_send(&ch, &resp), KIVFS_OK);
        kivfs_pkt_free(&req);
        kivfs_pkt_free(&resp);
        break;
    case SRV_CLOSE:
        CHECK_RC(kivfs_chan_recv(&ch, &req), KIVFS_OK);
        kivfs_pkt_free(&req);
        break;
    case SRV_HALF_PACKET:
    case SRV_HEADER_ONLY: {
        CHECK_RC(kivfs_chan_recv(&ch, &req), KIVFS_OK);
        kivfs_pkt_free(&req);
        memset(&h, 0, sizeof h);
        h.magic_number = KIVFS_MAGIC;
        h.request_code = kivfs_proto_response_code(REQ_ECHO);
        h.data_size = 10;
        kivfs_proto_header_encode(&h, hdr);
        struct iovec iov[2] = { { hdr, sizeof hdr }, { "abc", 3 } };
        CHECK_RC(kivfs_sock_writev_full(fd, iov, ctx->mode == SRV_HALF_PACKET ? 2 : 1),
                 KIVFS_OK);
        break;
    }
    case SRV_BAD_MAGIC: {
        CHECK_RC(kivfs_chan_recv(&ch, &req), KIVFS_OK);
        kivfs_pkt_free(&req);
        memset(hdr, 0xAB, sizeof hdr);
        struct iovec iov[1] = { { hdr, sizeof hdr } };
        CHECK_RC(kivfs_sock_writev_full(fd, iov, 1), KIVFS_OK);
        break;
    }
    case SRV_SLOW:
        CHECK_RC(kivfs_chan_recv(&ch, &req), KIVFS_OK);
        kivfs_pkt_free(&req);
        sleep_ms(600);
        break;
    case SRV_NONBLOCK:
        break;
    }

    kivfs_chan_close(&ch);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Klientské scénáře                                                   */
/* ------------------------------------------------------------------ */

static uint16_t g_port;
static int g_listen_fd = -1;

/* Spustí server v daném režimu a připojí k němu klienta. */
static pthread_t start_scenario(struct server_ctx *ctx, enum server_mode mode,
                                struct kivfs_chan *ch, int timeout_ms)
{
    pthread_t th;
    ctx->listen_fd = g_listen_fd;
    ctx->mode = mode;
    pthread_create(&th, NULL, server_main, ctx);
    kivfs_chan_init(ch);
    CHECK_RC(kivfs_chan_open(ch, "127.0.0.1", g_port, timeout_ms), KIVFS_OK);
    return th;
}

static void test_echo_and_multiple_calls(void)
{
    struct server_ctx ctx;
    struct kivfs_chan ch;
    pthread_t th = start_scenario(&ctx, SRV_ECHO, &ch, TIMEOUT_MS);
    CHECK(strncmp(ch.peer_addr, "127.0.0.1:", 10) == 0);

    /* Několik požadavků za sebou na jednom kanálu. */
    for (uint64_t ts = 1; ts <= 3; ts++) {
        struct kivfs_packet req, resp;
        kivfs_pkt_init(&req, REQ_ECHO, ts);
        CHECK_RC(kivfs_pkt_add_string(&req, "/kivfs"), KIVFS_OK);
        CHECK_RC(kivfs_pkt_add_u64(&req, ts * 1000), KIVFS_OK);

        CHECK_RC(kivfs_chan_call(&ch, &req, &resp), KIVFS_OK);
        CHECK(resp.header.return_value == KIVFS_STATUS_OK);

        struct kivfs_pkt_reader r;
        uint64_t ref, num;
        char path[16];
        kivfs_pkt_reader_init(&r, &resp);
        CHECK_RC(kivfs_pkt_read_u64(&r, &ref), KIVFS_OK);
        CHECK(ref == ts);
        CHECK_RC(kivfs_pkt_read_string(&r, path, sizeof path), KIVFS_OK);
        CHECK(strcmp(path, "/kivfs") == 0);
        CHECK_RC(kivfs_pkt_read_u64(&r, &num), KIVFS_OK);
        CHECK(num == ts * 1000);
        CHECK(kivfs_pkt_reader_at_end(&r));

        kivfs_pkt_free(&req);
        kivfs_pkt_free(&resp);
    }

    /* Velký payload (8 MiB) – vynutí částečná čtení i zápisy v jádře. */
    const uint32_t big_len = 8u * 1024u * 1024u;
    uint8_t *big = malloc(big_len);
    CHECK(big != NULL);
    if (big != NULL) {
        for (uint32_t i = 0; i < big_len; i++)
            big[i] = (uint8_t)(i * 31u + 7u);

        struct kivfs_packet req, resp;
        kivfs_pkt_init(&req, REQ_ECHO, 99);
        CHECK_RC(kivfs_pkt_add_bytes(&req, big, big_len), KIVFS_OK);
        CHECK_RC(kivfs_chan_call(&ch, &req, &resp), KIVFS_OK);

        struct kivfs_pkt_reader r;
        uint64_t ref;
        const uint8_t *data;
        uint32_t len = 0;
        kivfs_pkt_reader_init(&r, &resp);
        CHECK_RC(kivfs_pkt_read_u64(&r, &ref), KIVFS_OK);
        CHECK_RC(kivfs_pkt_read_bytes(&r, &data, &len), KIVFS_OK);
        CHECK(len == big_len && memcmp(data, big, big_len) == 0);

        kivfs_pkt_free(&req);
        kivfs_pkt_free(&resp);
        free(big);
    }

    kivfs_chan_close(&ch);
    kivfs_chan_close(&ch);              /* idempotentní */
    CHECK(ch.fd == -1);
    pthread_join(th, NULL);
}

/* Pošle jeden požadavek a ověří, jakou chybu vrátí call. */
static void expect_call_error(enum server_mode mode, int expected, int timeout_ms)
{
    struct server_ctx ctx;
    struct kivfs_chan ch;
    pthread_t th = start_scenario(&ctx, mode, &ch, timeout_ms);

    struct kivfs_packet req, resp;
    kivfs_pkt_init(&req, REQ_ECHO, 7);
    CHECK_RC(kivfs_pkt_add_string(&req, "x"), KIVFS_OK);
    CHECK_RC(kivfs_chan_call(&ch, &req, &resp), expected);
    CHECK(resp.data == NULL && resp.header.data_size == 0);   /* nic k uvolnění */

    kivfs_pkt_free(&req);
    kivfs_chan_close(&ch);
    pthread_join(th, NULL);
}

static void test_error_scenarios(void)
{
    expect_call_error(SRV_WRONG_REF,   KIVFS_EUNEXPECTED, TIMEOUT_MS);
    expect_call_error(SRV_CLOSE,       KIVFS_ECLOSED,     TIMEOUT_MS);
    expect_call_error(SRV_HALF_PACKET, KIVFS_ETRUNC,      TIMEOUT_MS);
    expect_call_error(SRV_HEADER_ONLY, KIVFS_ETRUNC,      TIMEOUT_MS);
    expect_call_error(SRV_BAD_MAGIC,   KIVFS_EBADMAGIC,   TIMEOUT_MS);
    expect_call_error(SRV_SLOW,        KIVFS_ETIMEOUT,    200);
}

static void test_nonblocking_api(void)
{
    /* Nikdo se nepřipojuje -> accept na neblokujícím listen fd vrátí EAGAIN. */
    CHECK_RC(kivfs_sock_accept(g_listen_fd, NULL, 0), KIVFS_EAGAIN);

    struct server_ctx ctx;
    struct kivfs_chan ch;
    pthread_t th = start_scenario(&ctx, SRV_NONBLOCK, &ch, TIMEOUT_MS);

    uint8_t b = 0;
    CHECK_RC(kivfs_sock_read_full(ch.fd, &b, 1), KIVFS_OK);
    CHECK(b == 'x');
    CHECK_RC(kivfs_sock_read_full(ch.fd, &b, 1), KIVFS_ECLOSED);
    pthread_join(th, NULL);

    /* Zápis do spojení zavřeného protistranou: chyba, ne pád na SIGPIPE.
     * (První zápis často ještě projde do bufferu jádra, proto víc pokusů.) */
    struct kivfs_packet p;
    kivfs_pkt_init(&p, REQ_ECHO, 1);
    int rc = KIVFS_OK;
    for (int i = 0; i < 20 && rc == KIVFS_OK; i++) {
        rc = kivfs_chan_send(&ch, &p);
        sleep_ms(10);
    }
    CHECK_RC(rc, KIVFS_EIO);
    kivfs_chan_close(&ch);

    /* Operace na zavřeném kanálu. */
    CHECK_RC(kivfs_chan_send(&ch, &p), KIVFS_EIO);
    CHECK_RC(kivfs_chan_recv(&ch, &p), KIVFS_EIO);
}

static void test_connect_failures(void)
{
    /* Port, na kterém nikdo neposlouchá: otevřít listener a hned zavřít. */
    int tmp = kivfs_sock_listen("127.0.0.1", 0, 1);
    CHECK(tmp >= 0);
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    getsockname(tmp, (struct sockaddr *)&sa, &sl);
    uint16_t dead_port = ntohs(sa.sin_port);
    kivfs_sock_close(&tmp);
    CHECK(tmp == -1);

    struct kivfs_chan ch;
    kivfs_chan_init(&ch);
    CHECK_RC(kivfs_chan_open(&ch, "127.0.0.1", dead_port, TIMEOUT_MS), KIVFS_EIO);
    CHECK(errno == ECONNREFUSED);
    CHECK(ch.fd == -1);

    /* Doména .invalid je rezervovaná a nikdy se nepřeloží (RFC 6761). */
    CHECK_RC(kivfs_chan_open(&ch, "kivfs.invalid", 9000, TIMEOUT_MS), KIVFS_ERESOLVE);
}

int main(void)
{
    CHECK_RC(kivfs_sock_ignore_sigpipe(), KIVFS_OK);

    /* Port 0 = jádro přidělí volný port; zjistíme ho přes getsockname. */
    g_listen_fd = kivfs_sock_listen("127.0.0.1", 0, 16);
    CHECK(g_listen_fd >= 0);
    if (g_listen_fd < 0) {
        fprintf(stderr, "listen selhal: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    getsockname(g_listen_fd, (struct sockaddr *)&sa, &sl);
    g_port = ntohs(sa.sin_port);

    test_echo_and_multiple_calls();
    test_error_scenarios();
    test_nonblocking_api();
    test_connect_failures();

    kivfs_sock_close(&g_listen_fd);
    printf("%d checks, %d failed\n", (int)g_checks, (int)g_failed);
    return g_failed == 0 ? 0 : 1;
}
