#include "protocol.h"
#include "errors.h"

/* Offsety polí hlavičky na drátě – viz tabulka v protocol.h. */
enum {
    OFF_MAGIC        = 0,
    OFF_TIMESTAMP    = 4,
    OFF_REQUEST_CODE = 12,
    OFF_RETURN_VALUE = 14,
    OFF_DATA_SIZE    = 16
};

void kivfs_proto_header_encode(const struct kivfs_header *h,
                               uint8_t out[KIVFS_HEADER_WIRE_SIZE])
{
    kivfs_proto_put_u32(out + OFF_MAGIC,        h->magic_number);
    kivfs_proto_put_u64(out + OFF_TIMESTAMP,    h->timestamp);
    kivfs_proto_put_u16(out + OFF_REQUEST_CODE, h->request_code);
    /* int16 -> uint16: převod záporného čísla na unsigned je v C definovaný
     * (modulo 2^16), takže -7 se zapíše jako 0xFFF9 (two's complement). */
    kivfs_proto_put_u16(out + OFF_RETURN_VALUE, (uint16_t)h->return_value);
    kivfs_proto_put_u32(out + OFF_DATA_SIZE,    h->data_size);
}

int kivfs_proto_header_decode(const uint8_t in[KIVFS_HEADER_WIRE_SIZE],
                              struct kivfs_header *h)
{
    h->magic_number = kivfs_proto_get_u32(in + OFF_MAGIC);
    if (h->magic_number != KIVFS_MAGIC)
        return KIVFS_EBADMAGIC;

    h->timestamp    = kivfs_proto_get_u64(in + OFF_TIMESTAMP);
    h->request_code = kivfs_proto_get_u16(in + OFF_REQUEST_CODE);

    /* uint16 -> int16: opačný směr (hodnota > 32767 do signed typu) je před
     * C23 "implementation-defined", proto převádíme ručně přes int32. */
    uint16_t rv = kivfs_proto_get_u16(in + OFF_RETURN_VALUE);
    h->return_value = (int16_t)(rv >= 0x8000u ? (int32_t)rv - 0x10000 : (int32_t)rv);

    h->data_size = kivfs_proto_get_u32(in + OFF_DATA_SIZE);
    if (h->data_size > KIVFS_MAX_PAYLOAD)
        return KIVFS_ETOOBIG;

    return KIVFS_OK;
}

const char *kivfs_status_str(int16_t status)
{
    switch (status) {
    case KIVFS_STATUS_OK:            return "OK";
    case KIVFS_STATUS_UNKNOWN_REQ:   return "UNKNOWN_REQ";
    case KIVFS_STATUS_MALFORMED:     return "MALFORMED";
    case KIVFS_STATUS_NOT_FOUND:     return "NOT_FOUND";
    case KIVFS_STATUS_EXISTS:        return "EXISTS";
    case KIVFS_STATUS_ACCESS_DENIED: return "ACCESS_DENIED";
    case KIVFS_STATUS_BUSY:          return "BUSY";
    case KIVFS_STATUS_INTERNAL:      return "INTERNAL";
    case KIVFS_STATUS_REJECTED:      return "REJECTED";
    }
    return "UNKNOWN_STATUS";
}
