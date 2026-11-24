#include "proto/packet.h"
#include "proto/errors.h"

#include <stdlib.h>
#include <string.h>

/* Počáteční velikost bufferu při prvním přidání záznamu. Malé pakety
 * (HELLO, krátká cesta) se tak vejdou do jediné alokace. */
#define PKT_MIN_CAPACITY 64u

/* ================================================================== */
/* Paket + builder                                                     */
/* ================================================================== */

void kivfs_pkt_init(struct kivfs_packet *p, uint16_t request_code, uint64_t timestamp)
{
    p->header.magic_number = KIVFS_MAGIC;
    p->header.timestamp    = timestamp;
    p->header.request_code = request_code;
    p->header.return_value = 0;
    p->header.data_size    = 0;
    p->data     = NULL;
    p->capacity = 0;
}

void kivfs_pkt_free(struct kivfs_packet *p)
{
    free(p->data);              /* free(NULL) je v C bezpečné */
    p->data = NULL;
    p->capacity = 0;
    p->header.data_size = 0;
}

/* Zajistí, že se do bufferu vejde dalších `extra` bajtů.
 * Při chybě paket NEMĚNÍ (realloc při selhání původní blok ponechá).
 * extra je uint64_t, aby součet 4 + len (len až 2^32-1) nepřetekl. */
static int pkt_reserve(struct kivfs_packet *p, uint64_t extra)
{
    uint64_t need = (uint64_t)p->header.data_size + extra;
    if (need > KIVFS_MAX_PAYLOAD)
        return KIVFS_ETOOBIG;
    if (need <= p->capacity)
        return KIVFS_OK;

    /* Geometrický růst (×2): N přidání stojí O(log N) realloců. need je
     * nejvýš KIVFS_MAX_PAYLOAD, takže zdvojování nepřeteče. */
    size_t new_cap = p->capacity ? p->capacity : PKT_MIN_CAPACITY;
    while (new_cap < need)
        new_cap *= 2;
    if (new_cap > KIVFS_MAX_PAYLOAD)
        new_cap = KIVFS_MAX_PAYLOAD;

    uint8_t *new_data = realloc(p->data, new_cap);
    if (new_data == NULL)
        return KIVFS_ENOMEM;

    p->data = new_data;
    p->capacity = new_cap;
    return KIVFS_OK;
}

int kivfs_pkt_add_bytes(struct kivfs_packet *p, const void *buf, uint32_t len)
{
    int rc = pkt_reserve(p, (uint64_t)KIVFS_RECORD_HDR_SIZE + len);
    if (rc != KIVFS_OK)
        return rc;

    uint8_t *dst = p->data + p->header.data_size;
    kivfs_proto_put_u32(dst, len);
    if (len > 0)                /* memcpy s NULL je UB i pro délku 0 */
        memcpy(dst + KIVFS_RECORD_HDR_SIZE, buf, len);

    /* data_size se mění až po úspěšném zápisu -> žádný napůl přidaný záznam */
    p->header.data_size += KIVFS_RECORD_HDR_SIZE + len;
    return KIVFS_OK;
}

int kivfs_pkt_add_string(struct kivfs_packet *p, const char *s)
{
    size_t len = strlen(s);
    if (len > KIVFS_MAX_PAYLOAD)   /* zároveň chrání přetypování na uint32_t */
        return KIVFS_ETOOBIG;
    return kivfs_pkt_add_bytes(p, s, (uint32_t)len);
}

int kivfs_pkt_add_u32(struct kivfs_packet *p, uint32_t v)
{
    uint8_t buf[4];
    kivfs_proto_put_u32(buf, v);
    return kivfs_pkt_add_bytes(p, buf, sizeof buf);
}

int kivfs_pkt_add_u64(struct kivfs_packet *p, uint64_t v)
{
    uint8_t buf[8];
    kivfs_proto_put_u64(buf, v);
    return kivfs_pkt_add_bytes(p, buf, sizeof buf);
}

int kivfs_pkt_init_response(struct kivfs_packet *resp, const struct kivfs_packet *req,
                            uint64_t timestamp, int16_t status)
{
    kivfs_pkt_init(resp, kivfs_proto_response_code(req->header.request_code), timestamp);
    resp->header.return_value = status;

    int rc = kivfs_pkt_add_u64(resp, req->header.timestamp);   /* ref */
    if (rc != KIVFS_OK)
        kivfs_pkt_free(resp);
    return rc;
}

int kivfs_pkt_response_ref(const struct kivfs_packet *resp, uint64_t *req_timestamp)
{
    if (!kivfs_proto_is_response(resp->header.request_code))
        return KIVFS_EMALFORMED;

    /* Vlastní lokální reader -> nezávisí na readeru volajícího. */
    struct kivfs_pkt_reader r;
    kivfs_pkt_reader_init(&r, resp);
    return kivfs_pkt_read_u64(&r, req_timestamp) == KIVFS_OK ? KIVFS_OK : KIVFS_EMALFORMED;
}

/* ================================================================== */
/* Reader                                                              */
/* Invariant: r->pos <= r->size, takže r->size - r->pos nikdy nepřeteče. */
/* ================================================================== */

void kivfs_pkt_reader_init(struct kivfs_pkt_reader *r, const struct kivfs_packet *p)
{
    r->data = p->data;
    /* Obrana proti nekonzistentnímu paketu (data == NULL, ale data_size > 0). */
    r->size = p->data ? p->header.data_size : 0;
    r->pos  = 0;
}

/* Najde další záznam a ověří, že leží celý uvnitř payloadu.
 * NEPOSOUVÁ pozici – posun dělá volající až po úspěchu (reader_advance),
 * takže při jakékoli chybě (i ETYPE) zůstane reader beze změny. */
static int reader_peek(const struct kivfs_pkt_reader *r, const uint8_t **out, uint32_t *len)
{
    uint32_t remaining = r->size - r->pos;

    if (remaining == 0)
        return KIVFS_EEND;
    if (remaining < KIVFS_RECORD_HDR_SIZE)        /* useknutá délka záznamu */
        return KIVFS_EMALFORMED;

    uint32_t rec_len = kivfs_proto_get_u32(r->data + r->pos);
    /* Porovnání odečtením místo pos + 4 + rec_len > size: součet by mohl
     * přetéct u32 (rec_len až 0xFFFFFFFF) a kontrola by pak "prošla". */
    if (rec_len > remaining - KIVFS_RECORD_HDR_SIZE)
        return KIVFS_EMALFORMED;

    *out = r->data + r->pos + KIVFS_RECORD_HDR_SIZE;
    *len = rec_len;
    return KIVFS_OK;
}

static void reader_advance(struct kivfs_pkt_reader *r, uint32_t rec_len)
{
    r->pos += KIVFS_RECORD_HDR_SIZE + rec_len;
}

int kivfs_pkt_read_bytes(struct kivfs_pkt_reader *r, const uint8_t **out, uint32_t *len)
{
    int rc = reader_peek(r, out, len);
    if (rc == KIVFS_OK)
        reader_advance(r, *len);
    return rc;
}

int kivfs_pkt_read_string(struct kivfs_pkt_reader *r, char *dst, size_t dst_size)
{
    const uint8_t *src;
    uint32_t len;
    int rc = reader_peek(r, &src, &len);
    if (rc != KIVFS_OK)
        return rc;

    if (dst_size == 0 || len > dst_size - 1)   /* -1 = místo pro '\0' */
        return KIVFS_ETYPE;

    if (len > 0)
        memcpy(dst, src, len);
    dst[len] = '\0';
    reader_advance(r, len);
    return KIVFS_OK;
}

int kivfs_pkt_read_u32(struct kivfs_pkt_reader *r, uint32_t *out)
{
    const uint8_t *src;
    uint32_t len;
    int rc = reader_peek(r, &src, &len);
    if (rc != KIVFS_OK)
        return rc;
    if (len != 4)
        return KIVFS_ETYPE;

    *out = kivfs_proto_get_u32(src);
    reader_advance(r, len);
    return KIVFS_OK;
}

int kivfs_pkt_read_u64(struct kivfs_pkt_reader *r, uint64_t *out)
{
    const uint8_t *src;
    uint32_t len;
    int rc = reader_peek(r, &src, &len);
    if (rc != KIVFS_OK)
        return rc;
    if (len != 8)
        return KIVFS_ETYPE;

    *out = kivfs_proto_get_u64(src);
    reader_advance(r, len);
    return KIVFS_OK;
}

int kivfs_pkt_reader_at_end(const struct kivfs_pkt_reader *r)
{
    return r->pos == r->size;
}
