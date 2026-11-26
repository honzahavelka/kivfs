/*
 * KIVFS – Sync: strana příjemce zpráv od Synců jiných uzlů (viz sync.h,
 * „PŘÍJEMCE“). Odpověď se posílá vždy hned – nikdy nečeká na frontu.
 */
#include "sync/sync.h"
#include "proto/errors.h"
#include "proto/protocol.h"

/* Odpověď [ref] + jedno u64 (PROPOSE). */
static void reply_u64(struct sync_state *st, const struct kivfs_mod_peer *from,
                      const struct kivfs_packet *req, uint64_t v)
{
    struct kivfs_packet resp;
    if (kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(st->m), KIVFS_STATUS_OK) != KIVFS_OK)
        return;
    if (kivfs_pkt_add_u64(&resp, v) != KIVFS_OK) {
        kivfs_pkt_free(&resp);
        kivfs_mod_reply_status(st->m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }
    kivfs_mod_reply(st->m, from->conn, &resp);
}

static void reply(struct sync_state *st, const struct kivfs_mod_peer *from,
                  const struct kivfs_packet *req, int16_t status)
{
    kivfs_mod_reply_status(st->m, from->conn, req, status);
}

/* Společné kontroly (sync.h): hlavička, epocha, u PROPOSE/COMMIT/ABORT
 * původ. Vrací 0 = OK, jinak už odpověděla chybou. */
static int check_hdr(struct sync_state *st, const struct kivfs_mod_peer *from,
                     const struct kivfs_packet *req, struct kivfs_pkt_reader *rd,
                     struct kivfs_sync_hdr *h, int origin_only)
{
    kivfs_pkt_reader_init(rd, req);
    if (kivfs_sync_hdr_read(rd, h) != KIVFS_OK) {
        reply(st, from, req, KIVFS_STATUS_MALFORMED);
        return -1;
    }
    if (h->epoch != st->epoch) {
        reply(st, from, req, KIVFS_STATUS_VIEW_MISMATCH);
        return -1;
    }
    if (origin_only && (h->op.node != from->node || h->op.incarnation != from->incarnation)) {
        kivfs_mod_log(st->m, "zpráva %u od node%u za cizí operaci node%u/%016llx – odmítnuto",
                      (unsigned)req->header.request_code, (unsigned)from->node,
                      (unsigned)h->op.node, (unsigned long long)h->op.incarnation);
        reply(st, from, req, KIVFS_STATUS_MALFORMED);
        return -1;
    }
    return 0;
}

int sync_on_propose(struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    struct kivfs_sync_hdr h;
    const uint8_t *payload = NULL;
    uint32_t len = 0;
    uint64_t t;

    if (check_hdr(st, from, req, &rd, &h, 1) != 0)
        return 1;
    if (h.lane == KIVFS_LANE_DB && kivfs_dbop_skip(&rd, &payload, &len) != KIVFS_OK) {
        reply(st, from, req, KIVFS_STATUS_MALFORMED);
        return 1;
    }
    if (!kivfs_pkt_reader_at_end(&rd)) {
        reply(st, from, req, KIVFS_STATUS_MALFORMED);
        return 1;
    }

    /* Neověřený Sync (po startu) o DB operacích nehlasuje – jinak by
     * ověření nepoznalo, co mu chybí (sync.h, „START A OVĚŘENÍ“). */
    if (h.lane == KIVFS_LANE_DB && !st->db.verified) {
        reply(st, from, req, KIVFS_STATUS_UNAVAILABLE);
        return 1;
    }

    /* Pozdní PROPOSE operace, o které už je rozhodnuto (nemá nastat –
     * původce PROPOSE neopakuje): nezařazovat, jinak by visela navždy. */
    const struct sync_recent *rec = sync_recent_find(st, h.lane, &h.op);
    if (rec != NULL) {
        if (rec->state == KIVFS_OPST_APPLIED)
            reply_u64(st, from, req, rec->t);
        else
            reply(st, from, req, KIVFS_STATUS_UNAVAILABLE);
        return 1;
    }

    int rc = kivfs_lane_on_propose(st->lane[h.lane], &h.op, h.t, payload, len, &t);
    switch (rc) {
    case KIVFS_OK:     reply_u64(st, from, req, t);                     break;
    case KIVFS_EAGAIN: reply(st, from, req, KIVFS_STATUS_BUSY);         break;
    case KIVFS_EINVAL: reply(st, from, req, KIVFS_STATUS_MALFORMED);    break;
    default:           reply(st, from, req, KIVFS_STATUS_INTERNAL);     break;
    }
    return 1;
}

int sync_on_commit(struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    struct kivfs_sync_hdr h;

    if (check_hdr(st, from, req, &rd, &h, 1) != 0)
        return 1;
    int rc = kivfs_lane_commit(st->lane[h.lane], &h.op, h.t);
    if (rc == KIVFS_EPROTO) {
        kivfs_mod_log(st->m, "COMMIT od node%u porušuje protokol (op seq %llu, t %llu)",
                      (unsigned)from->node, (unsigned long long)h.op.seq, (unsigned long long)h.t);
        reply(st, from, req, KIVFS_STATUS_INTERNAL);
        return 1;
    }
    if (rc == KIVFS_ENOENT && h.lane == KIVFS_LANE_DB
        && sync_recent_find(st, h.lane, &h.op) == NULL)
        /* operaci neznám (hlasovala o ní moje předchozí inkarnace, nebo se
         * PROPOSE ztratil) a provést ji neumím – další index by patřil jiné
         * operaci -> zastavit. Původci OK: COMMIT jsem dostal. */
        sync_db_stop(st, "COMMIT neznámé DB operace node%u/%llu",
                     (unsigned)h.op.node, (unsigned long long)h.op.seq);
    /* ENOENT u pruhu CLI je běžné (cizí operace se nedrží) */
    reply(st, from, req, KIVFS_STATUS_OK);
    sync_execute_next(st, h.lane);
    return 1;
}

int sync_on_abort(struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    struct kivfs_sync_hdr h;

    if (check_hdr(st, from, req, &rd, &h, 1) != 0)
        return 1;
    int rc = kivfs_lane_abort(st->lane[h.lane], &h.op, NULL);
    if (rc == KIVFS_EPROTO) {
        kivfs_mod_log(st->m, "ABORT potvrzené operace od node%u (op seq %llu)",
                      (unsigned)from->node, (unsigned long long)h.op.seq);
        reply(st, from, req, KIVFS_STATUS_INTERNAL);
        return 1;
    }
    if (rc == KIVFS_OK || sync_recent_find(st, h.lane, &h.op) == NULL)
        sync_recent_add(st, h.lane, &h.op, KIVFS_OPST_UNKNOWN, 0);   /* proti pozdnímu PROPOSE */
    reply(st, from, req, KIVFS_STATUS_OK);
    sync_execute_next(st, h.lane);
    return 1;
}

int sync_on_query(struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    struct kivfs_sync_hdr h;
    struct kivfs_sync_state s = { KIVFS_OPST_UNKNOWN, 0 };

    if (check_hdr(st, from, req, &rd, &h, 0) != 0)
        return 1;
    const struct kivfs_lane_entry *e = kivfs_lane_find(st->lane[h.lane], &h.op);
    if (e != NULL) {
        s.state = e->state == KIVFS_LANE_PROPOSED ? KIVFS_OPST_PROPOSED : KIVFS_OPST_COMMITTED;
        s.t     = e->state == KIVFS_LANE_PROPOSED ? e->t_proposed : e->t;
    } else {
        const struct sync_recent *rec = sync_recent_find(st, h.lane, &h.op);
        if (rec != NULL) {
            s.state = rec->state;
            s.t     = rec->t;
        }
    }

    struct kivfs_packet resp;
    if (kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(st->m), KIVFS_STATUS_OK) != KIVFS_OK)
        return 1;
    if (kivfs_sync_state_add(&resp, &s) != KIVFS_OK) {
        kivfs_pkt_free(&resp);
        reply(st, from, req, KIVFS_STATUS_INTERNAL);
        return 1;
    }
    kivfs_mod_reply(st->m, from->conn, &resp);
    return 1;
}

/* SYNC_CLOCK req [] -> [ref][sync_clock]: ověření po startu jiného
 * uzlu. Neznám-li ještě head své DB, last_index nic neříká -> UNAVAILABLE
 * (tazatel to zkusí znovu). */
int sync_on_clock(struct sync_state *st, const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    kivfs_pkt_reader_init(&rd, req);
    if (!kivfs_pkt_reader_at_end(&rd)) {
        reply(st, from, req, KIVFS_STATUS_MALFORMED);
        return 1;
    }
    if (!st->db.head_known) {
        reply(st, from, req, KIVFS_STATUS_UNAVAILABLE);
        return 1;
    }
    struct kivfs_sync_clock c = {
        kivfs_lane_clock(st->lane[KIVFS_LANE_CLI]),
        kivfs_lane_clock(st->lane[KIVFS_LANE_DB]),
        kivfs_lane_last_index(st->lane[KIVFS_LANE_DB]),
        kivfs_lane_count(st->lane[KIVFS_LANE_DB]) };
    struct kivfs_packet resp;
    if (kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(st->m), KIVFS_STATUS_OK) != KIVFS_OK)
        return 1;
    if (kivfs_sync_clock_add(&resp, &c) != KIVFS_OK) {
        kivfs_pkt_free(&resp);
        reply(st, from, req, KIVFS_STATUS_INTERNAL);
        return 1;
    }
    kivfs_mod_reply(st->m, from->conn, &resp);
    return 1;
}
