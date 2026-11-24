/*
 * KIVFS – HELLO (implementace), viz hello.h
 */
#define _POSIX_C_SOURCE 200809L

#include "hello.h"
#include "errors.h"
#include "protocol.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int add_fields(struct kivfs_packet *p, const struct kivfs_hello *h)
{
    int rc;
    if ((rc = kivfs_pkt_add_u32(p, h->node_id))      != KIVFS_OK) return rc;
    if ((rc = kivfs_pkt_add_u32(p, h->module))       != KIVFS_OK) return rc;
    if ((rc = kivfs_pkt_add_u32(p, h->version))      != KIVFS_OK) return rc;
    if ((rc = kivfs_pkt_add_u64(p, h->incarnation))  != KIVFS_OK) return rc;
    return kivfs_pkt_add_u32(p, h->cluster_hash);
}

static int read_fields(struct kivfs_pkt_reader *r, struct kivfs_hello *h)
{
    if (kivfs_pkt_read_u32(r, &h->node_id)     != KIVFS_OK
        || kivfs_pkt_read_u32(r, &h->module)   != KIVFS_OK
        || kivfs_pkt_read_u32(r, &h->version)  != KIVFS_OK
        || kivfs_pkt_read_u64(r, &h->incarnation) != KIVFS_OK
        || kivfs_pkt_read_u32(r, &h->cluster_hash) != KIVFS_OK
        || !kivfs_pkt_reader_at_end(r))
        return KIVFS_EMALFORMED;
    return KIVFS_OK;
}

int kivfs_hello_request(struct kivfs_packet *req, uint64_t ts, const struct kivfs_hello *me)
{
    kivfs_pkt_init(req, KIVFS_REQ_HELLO, ts);
    int rc = add_fields(req, me);
    if (rc != KIVFS_OK)
        kivfs_pkt_free(req);
    return rc;
}

int kivfs_hello_response(struct kivfs_packet *resp, const struct kivfs_packet *req,
                         uint64_t ts, int16_t status, const struct kivfs_hello *me)
{
    int rc = kivfs_pkt_init_response(resp, req, ts, status);
    if (rc != KIVFS_OK)
        return rc;
    rc = add_fields(resp, me);
    if (rc != KIVFS_OK)
        kivfs_pkt_free(resp);
    return rc;
}

int kivfs_hello_parse_request(const struct kivfs_packet *req, struct kivfs_hello *out)
{
    struct kivfs_pkt_reader r;
    kivfs_pkt_reader_init(&r, req);
    return read_fields(&r, out);
}

int kivfs_hello_parse_response(const struct kivfs_packet *resp, struct kivfs_hello *out)
{
    struct kivfs_pkt_reader r;
    uint64_t ref;
    kivfs_pkt_reader_init(&r, resp);
    if (kivfs_pkt_read_u64(&r, &ref) != KIVFS_OK)
        return KIVFS_EMALFORMED;
    return read_fields(&r, out);
}

int kivfs_module_may_call(uint32_t caller, uint32_t callee)
{
    switch (caller) {
    case KIVFS_MOD_CLIENT: return callee == KIVFS_MOD_AUTH;
    case KIVFS_MOD_AUTH:   return callee == KIVFS_MOD_SYNC;
    case KIVFS_MOD_SYNC:   return callee == KIVFS_MOD_VFS || callee == KIVFS_MOD_SYNC;
    case KIVFS_MOD_VFS:    return callee == KIVFS_MOD_DB || callee == KIVFS_MOD_FS;
    default:               return 0;
    }
}

static uint32_t fnv_u32(uint32_t h, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        h ^= (v >> shift) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

uint32_t kivfs_cluster_hash(const uint32_t *ids_sorted, size_t n)
{
    uint32_t h = 2166136261u;
    h = fnv_u32(h, (uint32_t)n);
    for (size_t i = 0; i < n; i++)
        h = fnv_u32(h, ids_sorted[i]);
    return h != 0 ? h : 1;
}

uint64_t kivfs_incarnation_new(void)
{
    uint64_t v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        if (fread(&v, sizeof v, 1, f) != 1)
            v = 0;
        fclose(f);
    }
    if (v == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        v = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 16);
    }
    return v != 0 ? v : 1;
}
