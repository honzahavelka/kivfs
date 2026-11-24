/* poll(): stejné makro jako v socket.c (na macOS by čisté POSIX makro
 * schovalo i věci, které potřebují systémové hlavičky). */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include "net/channel.h"
#include "proto/errors.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

void kivfs_chan_init(struct kivfs_chan *c)
{
    c->fd = -1;
    c->peer_node_id = KIVFS_NODE_ID_UNKNOWN;
    c->peer_addr[0] = '\0';
}

void kivfs_chan_close(struct kivfs_chan *c)
{
    kivfs_sock_close(&c->fd);
    c->peer_node_id = KIVFS_NODE_ID_UNKNOWN;
    c->peer_addr[0] = '\0';
}

int kivfs_chan_open(struct kivfs_chan *c, const char *host, uint16_t port, int timeout_ms)
{
    kivfs_chan_close(c);        /* znovuotevření kanálu nesmí nechat viset starý fd */

    int fd = kivfs_sock_connect(host, port, timeout_ms);
    if (fd < 0)
        return fd;              /* záporné číslo = kód chyby */

    int rc = kivfs_sock_set_timeout(fd, timeout_ms);
    if (rc != KIVFS_OK) {
        kivfs_sock_close(&fd);
        return rc;
    }

    c->fd = fd;
    if (kivfs_sock_peer_str(fd, c->peer_addr, sizeof c->peer_addr) != KIVFS_OK)
        snprintf(c->peer_addr, sizeof c->peer_addr, "%s:%u", host, (unsigned)port);
    return KIVFS_OK;
}

int kivfs_chan_send(struct kivfs_chan *c, const struct kivfs_packet *p)
{
    if (c->fd < 0) {
        errno = EBADF;
        return KIVFS_EIO;
    }
    /* Paket z builderu je vždy konzistentní; tohle chytí ručně rozbitý. */
    if (p->header.data_size > KIVFS_MAX_PAYLOAD)
        return KIVFS_ETOOBIG;
    if (p->header.data_size > 0 && p->data == NULL)
        return KIVFS_EMALFORMED;

    uint8_t hdr[KIVFS_HEADER_WIRE_SIZE];
    kivfs_proto_header_encode(&p->header, hdr);

    struct iovec iov[2];
    int iovcnt = 1;
    iov[0].iov_base = hdr;
    iov[0].iov_len  = sizeof hdr;
    if (p->header.data_size > 0) {
        iov[1].iov_base = p->data;
        iov[1].iov_len  = p->header.data_size;
        iovcnt = 2;
    }
    return kivfs_sock_writev_full(c->fd, iov, iovcnt);
}

int kivfs_chan_recv(struct kivfs_chan *c, struct kivfs_packet *out)
{
    kivfs_pkt_init(out, 0, 0);  /* při jakékoli chybě vrátíme prázdný paket */

    if (c->fd < 0) {
        errno = EBADF;
        return KIVFS_EIO;
    }

    /* 1. hlavička */
    uint8_t hdr[KIVFS_HEADER_WIRE_SIZE];
    int rc = kivfs_sock_read_full(c->fd, hdr, sizeof hdr);
    if (rc != KIVFS_OK)
        return rc;              /* ECLOSED zde = čistý konec mezi pakety */

    /* 2. validace – při chybě se payload ani nealokuje */
    struct kivfs_header h;
    rc = kivfs_proto_header_decode(hdr, &h);
    if (rc != KIVFS_OK)
        return rc;

    /* 3. payload */
    uint8_t *data = NULL;
    if (h.data_size > 0) {
        data = malloc(h.data_size);
        if (data == NULL)
            return KIVFS_ENOMEM;

        rc = kivfs_sock_read_full(c->fd, data, h.data_size);
        if (rc != KIVFS_OK) {
            free(data);
            /* Hlavička už přišla, takže konec spojení před 1. bajtem
             * payloadu je z pohledu paketu useknutí, ne čistý konec. */
            return rc == KIVFS_ECLOSED ? KIVFS_ETRUNC : rc;
        }
    }

    out->header   = h;
    out->data     = data;
    out->capacity = h.data_size;
    return KIVFS_OK;
}

int kivfs_chan_call(struct kivfs_chan *c, const struct kivfs_packet *req,
                    struct kivfs_packet *resp)
{
    int rc = kivfs_chan_send(c, req);
    if (rc != KIVFS_OK) {
        kivfs_pkt_init(resp, 0, 0);    /* resp vždy v definovaném stavu */
        return rc;
    }

    return kivfs_chan_recv_response(c, req, resp);
}

int kivfs_chan_recv_response(struct kivfs_chan *c, const struct kivfs_packet *req,
                             struct kivfs_packet *resp)
{
    int rc = kivfs_chan_recv(c, resp);
    if (rc != KIVFS_OK)
        return rc;

    /* Patří odpověď k našemu požadavku? */
    uint64_t ref;
    if (!kivfs_proto_is_response(resp->header.request_code)
        || kivfs_proto_base_code(resp->header.request_code) != req->header.request_code
        || kivfs_pkt_response_ref(resp, &ref) != KIVFS_OK
        || ref != req->header.timestamp) {
        kivfs_pkt_free(resp);
        return KIVFS_EUNEXPECTED;
    }
    return KIVFS_OK;
}

int kivfs_chan_is_stale(const struct kivfs_chan *c)
{
    if (c->fd < 0)
        return 1;
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN, .revents = 0 };
    int n;
    do {
        n = poll(&pfd, 1, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return 1;               /* nejde ani zjistit stav -> nevěřit mu */
    return n > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
}
