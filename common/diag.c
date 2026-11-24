/*
 * KIVFS – diagnostika propojení (implementace), viz diag.h
 */
#include "diag.h"
#include "errors.h"

#define TRY(x) do { int rc_ = (x); if (rc_ != KIVFS_OK) return rc_; } while (0)

int kivfs_diag_add(struct kivfs_packet *p, const struct kivfs_diag_module *m)
{
    uint32_t n_out = m->n_out < KIVFS_DIAG_MAX_LINKS ? m->n_out : KIVFS_DIAG_MAX_LINKS;
    uint32_t n_in  = m->n_in  < KIVFS_DIAG_MAX_LINKS ? m->n_in  : KIVFS_DIAG_MAX_LINKS;

    TRY(kivfs_pkt_add_u32(p, m->node));
    TRY(kivfs_pkt_add_u32(p, m->module));
    TRY(kivfs_pkt_add_u64(p, m->incarnation));
    TRY(kivfs_pkt_add_u32(p, m->ready));
    TRY(kivfs_pkt_add_u32(p, n_out));
    for (uint32_t i = 0; i < n_out; i++) {
        TRY(kivfs_pkt_add_u32(p, m->out[i].module));
        TRY(kivfs_pkt_add_u32(p, m->out[i].node));
        TRY(kivfs_pkt_add_u32(p, m->out[i].state));
        TRY(kivfs_pkt_add_u64(p, m->out[i].peer_incarnation));
    }
    TRY(kivfs_pkt_add_u32(p, n_in));
    for (uint32_t i = 0; i < n_in; i++) {
        TRY(kivfs_pkt_add_u32(p, m->in[i].module));
        TRY(kivfs_pkt_add_u32(p, m->in[i].node));
    }
    return KIVFS_OK;
}

int kivfs_diag_read(struct kivfs_pkt_reader *r, struct kivfs_diag_module *m)
{
    if (kivfs_pkt_reader_at_end(r))
        return KIVFS_EEND;

#define RD32(v) do { if (kivfs_pkt_read_u32(r, (v)) != KIVFS_OK) return KIVFS_EMALFORMED; } while (0)
#define RD64(v) do { if (kivfs_pkt_read_u64(r, (v)) != KIVFS_OK) return KIVFS_EMALFORMED; } while (0)
    RD32(&m->node);
    RD32(&m->module);
    RD64(&m->incarnation);
    RD32(&m->ready);
    RD32(&m->n_out);
    if (m->n_out > KIVFS_DIAG_MAX_LINKS)
        return KIVFS_EMALFORMED;
    for (uint32_t i = 0; i < m->n_out; i++) {
        RD32(&m->out[i].module);
        RD32(&m->out[i].node);
        RD32(&m->out[i].state);
        RD64(&m->out[i].peer_incarnation);
    }
    RD32(&m->n_in);
    if (m->n_in > KIVFS_DIAG_MAX_LINKS)
        return KIVFS_EMALFORMED;
    for (uint32_t i = 0; i < m->n_in; i++) {
        RD32(&m->in[i].module);
        RD32(&m->in[i].node);
    }
#undef RD32
#undef RD64
    return KIVFS_OK;
}

const char *kivfs_link_state_str(uint32_t state)
{
    switch (state) {
    case KIVFS_LINK_DOWN:       return "DOWN";
    case KIVFS_LINK_CONNECTING: return "CONNECTING";
    case KIVFS_LINK_UP:         return "UP";
    default:                    return "?";
    }
}
