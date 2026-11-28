/*
 * kivfs-db – modul DB (kap. 7.6): metadata a archiv operací.
 *
 * Čtení (DB_GET, DB_LIST) od VFS obsluhuje lokálně nad backendem
 * (db/backend.h). Zápisy (DB_MKNODE, DB_RMNODE) od VFS NEprovádí hned:
 * zabalí je do DB operace a pošle Syncu (SYNC_SUBMIT); Sync jim hlasováním
 * určí pořadí Tdb a na každém uzlu je pak pošle zpět jako DB_APPLY, které
 * DB provede a archivuje (MESSAGE_LOGS) v jednom volání backendu. Původci
 * Sync odpoví na SUBMIT výsledkem jeho vlastního APPLY a DB ho přebalí
 * pro VFS.
 *
 * Worker při čekání na SUBMIT NEBLOKUJE (kivfs_mod_call + pokračování) –
 * mezitím musí obsloužit DB_APPLY, i to své (architektura, bod 8).
 *
 * Kdo smí co (jinak ACCESS_DENIED):
 *   VFS   DB_GET, DB_LIST, DB_MKNODE, DB_RMNODE
 *   Sync  DB_APPLY, DB_LOG_READ, DB_LOG_HEAD
 *
 * Výsledek zápisu pro VFS:
 *   SUBMIT se neodeslal (linka na Sync dole / plno)  UNAVAILABLE / BUSY
 *                                                    (jistě neproběhl)
 *   po odeslání timeout / pád spojení / vypínání     TIMEOUT (NEZNÁMÝ –
 *                                                    kolo mohlo proběhnout)
 *   odpověď Syncu                                    její stav + záznamy
 *                                                    (= výsledek APPLY, nebo
 *                                                    UNAVAILABLE/BUSY kola)
 *
 * DB_APPLY [arch_hdr][DB operace] (viz backend.h, invariant apply):
 *   rozbitý rámec (nejde přečíst index / operaci)    INTERNAL, nic
 *   index == head a stejné op_id (opakování)         uložený stav, nic
 *   index != head + 1                                INTERNAL, nic
 *   vadný obsah operace (kód, záznamy)               MALFORMED, archivováno
 *   jinak výsledek apply; MKNODE OK -> [attr]
 *
 * Konfigurace, sekce [db]:
 *   backend = mock      (výchozí; strom v paměti, při restartu se ztratí)
 * Použití: kivfs-db -c <soubor.ini>
 */
#define _POSIX_C_SOURCE 200809L

#include "core/module.h"
#include "core/modconf.h"
#include "db/backend.h"
#include "proto/errors.h"
#include "proto/fsproto.h"
#include "proto/protocol.h"
#include "proto/syncproto.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Timeout čekání na SUBMIT (kivfs_modconf_timeouts: db_submit < VFS->DB
 * zápis; uvnitř kolo + potvrzení COMMIT + fronta pruhu DB + APPLY). */
static int submit_timeout_ms(const struct kivfs_mod *m)
{
    struct kivfs_timeouts t;
    kivfs_modconf_timeouts(kivfs_mod_conf(m), &t);
    return t.db_submit;
}

static int db_init(struct kivfs_mod *m, const struct kivfs_config *cfg)
{
    const char *kind = kivfs_config_get(cfg, "db", "backend");
    struct kivfs_dbb *b = NULL;
    if (kind == NULL || strcmp(kind, "mock") == 0) {
        if (kivfs_dbb_mock_open(&b) != KIVFS_OK)
            return -1;
    } else {
        fprintf(stderr, "kivfs-db: neznámý backend '%s' (zatím jen mock)\n", kind);
        return -1;
    }
    kivfs_mod_set_user(m, b);
    return 0;
}

static void db_fini(struct kivfs_mod *m)
{
    struct kivfs_dbb *b = kivfs_mod_user(m);
    if (b != NULL)
        b->ops->close(b);
}

/* ------------------------------------------------------------------ */
/* Pomocné                                                             */
/* ------------------------------------------------------------------ */

/* Odpověď se stavem st, při OK s [attr] (chyba přidání -> INTERNAL). */
static void reply_attr(struct kivfs_mod *m, kivfs_conn_id to, const struct kivfs_packet *req,
                       int16_t st, const struct kivfs_attr *a)
{
    struct kivfs_packet resp;
    if (st == KIVFS_STATUS_OK && a != NULL
        && kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(m), st) == KIVFS_OK) {
        if (kivfs_attr_add(&resp, a) == KIVFS_OK) {
            kivfs_mod_reply(m, to, &resp);
            return;
        }
        kivfs_pkt_free(&resp);
        st = KIVFS_STATUS_INTERNAL;
    }
    kivfs_mod_reply_status(m, to, req, st);
}

/* Rozbalí záznamy zápisu (za [dbop]) podle kódu: MKNODE [path][mknode],
 * RMNODE [path]. n = očekávaný počet záznamů (n_records z dbop), r musí
 * být po nich na konci. path má KIVFS_PATH_MAX + 1 B. 0 = OK, -1 = vadné. */
static int decode_write(uint32_t code, uint32_t n, struct kivfs_pkt_reader *r,
                        char *path, struct kivfs_dbb_write *w)
{
    memset(w, 0, sizeof *w);
    if (code == KIVFS_REQ_DB_MKNODE) {
        if (n != 2 || kivfs_path_read(r, path) != KIVFS_OK
            || kivfs_mknode_read(r, &w->mode, &w->uid, &w->gid) != KIVFS_OK)
            return -1;
        w->kind = KIVFS_DBB_W_MKNODE;
    } else if (code == KIVFS_REQ_DB_RMNODE) {
        if (n != 1 || kivfs_path_read(r, path) != KIVFS_OK)
            return -1;
        w->kind = KIVFS_DBB_W_RMNODE;
    } else {
        return -1;
    }
    if (!kivfs_pkt_reader_at_end(r))
        return -1;
    w->path = path;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Čtení od VFS                                                        */
/* ------------------------------------------------------------------ */

struct list_ctx {
    struct kivfs_packet *resp;
};

static int emit_entry(void *ud, uint64_t id, uint32_t mode, const char *name)
{
    struct list_ctx *l = ud;
    return kivfs_entry_add(l->resp, id, mode, name, strlen(name)) == KIVFS_OK ? 0 : 1;
}

static void on_read(struct kivfs_mod *m, struct kivfs_dbb *b,
                    const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    struct kivfs_packet resp;
    struct kivfs_attr a;
    char path[KIVFS_PATH_MAX + 1];
    int16_t st;

    kivfs_pkt_reader_init(&rd, req);
    if (kivfs_path_read(&rd, path) != KIVFS_OK || !kivfs_pkt_reader_at_end(&rd)) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_MALFORMED);
        return;
    }
    if (req->header.request_code == KIVFS_REQ_DB_GET) {
        st = b->ops->get(b, path, &a);
        reply_attr(m, from->conn, req, st, &a);
        return;
    }
    /* DB_LIST */
    if (kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(m), KIVFS_STATUS_OK) != KIVFS_OK) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }
    struct list_ctx l = { &resp };
    /* emit selže, když položky přerostou KIVFS_MAX_PAYLOAD -> INTERNAL */
    st = b->ops->list(b, path, emit_entry, &l);
    if (st == KIVFS_STATUS_OK) {
        kivfs_mod_reply(m, from->conn, &resp);
        return;
    }
    kivfs_pkt_free(&resp);
    kivfs_mod_reply_status(m, from->conn, req, st);
}

/* ------------------------------------------------------------------ */
/* Zápis od VFS -> SYNC_SUBMIT                                         */
/* ------------------------------------------------------------------ */

/* Rozpracovaný SUBMIT: komu a na co odpovědět (odpověď potřebuje jen
 * hlavičku původního požadavku – ref = jeho timestamp). */
struct submit {
    kivfs_conn_id       to;
    struct kivfs_header orig;
};

static void after_submit(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct submit *s = state;
    struct kivfs_packet orig = { .header = s->orig };    /* init_response čte jen hlavičku */
    struct kivfs_packet out;

    /* po odeslání: výsledek NEZNÁMÝ (kolo mohlo proběhnout a úklid ho
     * dokončí) -> TIMEOUT, nikdy UNAVAILABLE */
    int16_t st = err == KIVFS_OK ? resp->header.return_value : KIVFS_STATUS_TIMEOUT;
    if (err != KIVFS_OK)
        kivfs_mod_log(m, "SUBMIT bez odpovědi Syncu (%s) -> TIMEOUT", kivfs_strerror(err));

    if (kivfs_pkt_init_response(&out, &orig, kivfs_mod_next_ts(m), st) != KIVFS_OK) {
        free(s);
        return;
    }
    if (err == KIVFS_OK) {
        /* záznamy za ref Syncu (výsledek APPLY, u MKNODE [attr]) -> za náš ref */
        struct kivfs_pkt_reader rd;
        const uint8_t *rec;
        uint32_t len;
        uint64_t ref;
        kivfs_pkt_reader_init(&rd, resp);
        if (kivfs_pkt_read_u64(&rd, &ref) == KIVFS_OK)
            while (kivfs_pkt_read_bytes(&rd, &rec, &len) == KIVFS_OK)
                if (kivfs_pkt_add_bytes(&out, rec, len) != KIVFS_OK) {
                    /* nemůže nastat (odpověď Syncu se vešla), ale stav
                     * zápisu by se ztratil -> neznámý */
                    kivfs_pkt_free(&out);
                    kivfs_pkt_init_response(&out, &orig, kivfs_mod_next_ts(m),
                                            KIVFS_STATUS_TIMEOUT);
                    break;
                }
    }
    kivfs_mod_reply(m, s->to, &out);
    free(s);
}

static void on_write(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                     struct kivfs_packet *req)
{
    uint16_t code = req->header.request_code;
    struct kivfs_pkt_reader rd;
    struct kivfs_dbb_write w;
    char path[KIVFS_PATH_MAX + 1];
    uint32_t n = code == KIVFS_REQ_DB_MKNODE ? 2 : 1;

    /* kontrola obsahu u původce – na ostatní uzly jde jen platná operace */
    kivfs_pkt_reader_init(&rd, req);
    if (decode_write(code, n, &rd, path, &w) != 0) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_MALFORMED);
        return;
    }

    /* DB operace = [dbop code n time] + záznamy požadavku beze změny;
     * čas určuje původce (backend.h – determinismus) */
    struct kivfs_packet q;
    struct kivfs_dbop d = { code, n, now_ms() };
    const uint8_t *rec;
    uint32_t len;
    int rc;
    kivfs_pkt_init(&q, KIVFS_REQ_SYNC_SUBMIT, 0);
    kivfs_pkt_reader_init(&rd, req);
    rc = kivfs_dbop_add(&q, &d);
    while (rc == KIVFS_OK && kivfs_pkt_read_bytes(&rd, &rec, &len) == KIVFS_OK)
        rc = kivfs_pkt_add_bytes(&q, rec, len);
    if (rc == KIVFS_OK && q.header.data_size > KIVFS_MAX_PAYLOAD - KIVFS_SYNC_PAYLOAD_RESERVE)
        rc = KIVFS_ETOOBIG;
    if (rc != KIVFS_OK) {
        kivfs_pkt_free(&q);
        kivfs_mod_reply_status(m, from->conn, req, rc == KIVFS_ETOOBIG ? KIVFS_STATUS_MALFORMED
                                                                      : KIVFS_STATUS_INTERNAL);
        return;
    }

    struct submit *s = malloc(sizeof *s);
    if (s == NULL) {
        kivfs_pkt_free(&q);
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }
    s->to   = from->conn;
    s->orig = req->header;
    rc = kivfs_mod_call(m, KIVFS_MOD_SYNC, &q, submit_timeout_ms(m), after_submit, s);
    if (rc != KIVFS_OK) {
        /* neodesláno -> jistě neproběhlo */
        free(s);
        kivfs_mod_reply_status(m, from->conn, req,
                               rc == KIVFS_ENOTREADY ? KIVFS_STATUS_UNAVAILABLE
                               : rc == KIVFS_EAGAIN  ? KIVFS_STATUS_BUSY
                                                     : KIVFS_STATUS_INTERNAL);
    }
}

/* ------------------------------------------------------------------ */
/* Od Syncu: DB_APPLY, DB_LOG_HEAD, DB_LOG_READ                        */
/* ------------------------------------------------------------------ */

static void on_apply(struct kivfs_mod *m, struct kivfs_dbb *b,
                     const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd, op_rd;
    struct kivfs_arch_hdr ah;
    struct kivfs_dbop d = { 0 };
    struct kivfs_dbb_logrec head, rec;
    struct kivfs_dbb_write w;
    struct kivfs_attr a = { 0 };
    char path[KIVFS_PATH_MAX + 1];
    const uint8_t *op;
    uint32_t op_len;
    int16_t st;

    /* rámec: bez něj neznám index ani operaci -> INTERNAL, nic */
    kivfs_pkt_reader_init(&rd, req);
    if (kivfs_arch_hdr_read(&rd, &ah) != KIVFS_OK
        || kivfs_dbop_skip(&rd, &op, &op_len) != KIVFS_OK
        || !kivfs_pkt_reader_at_end(&rd)) {
        kivfs_mod_log(m, "DB_APPLY: rozbitý rámec -> INTERNAL");
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }

    /* obsah: vadný -> INVALID (archivuje se jako MALFORMED) */
    op_rd = (struct kivfs_pkt_reader){ op, op_len, 0 };
    if (kivfs_dbop_read(&op_rd, &d) != KIVFS_OK
        || decode_write(d.code, d.n_records, &op_rd, path, &w) != 0) {
        memset(&w, 0, sizeof w);
        w.kind = KIVFS_DBB_W_INVALID;
    }
    w.time_ms = d.time_ms;
    w.new_id  = kivfs_db_id_from_index(ah.index);

    if (b->ops->log_head(b, &head) != KIVFS_STATUS_OK) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }

    /* opakování poslední operace (ztracená odpověď) -> uložený výsledek;
     * strom se od ní nezměnil, takže atributy MKNODE dá get */
    if (ah.index != 0 && ah.index == head.index && kivfs_op_id_eq(&ah.op, &head.op)) {
        kivfs_mod_log(m, "DB_APPLY: opakování indexu %" PRIu64 " -> uložený výsledek",
                      ah.index);
        st = head.status;
        if (st == KIVFS_STATUS_OK && w.kind == KIVFS_DBB_W_MKNODE)
            st = b->ops->get(b, w.path, &a);
        reply_attr(m, from->conn, req, st,
                   w.kind == KIVFS_DBB_W_MKNODE ? &a : NULL);
        return;
    }
    if (ah.index != head.index + 1) {
        kivfs_mod_log(m, "DB_APPLY: index %" PRIu64 ", čekám %" PRIu64 " (%s) -> INTERNAL",
                      ah.index, head.index + 1,
                      ah.index <= head.index ? "duplicita" : "díra");
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }

    rec = (struct kivfs_dbb_logrec){ ah.index, ah.t, ah.op, 0, op, op_len };
    st = b->ops->apply(b, &w, &rec, &a);
    if (st == KIVFS_STATUS_INTERNAL)
        kivfs_mod_log(m, "DB_APPLY: index %" PRIu64 " selhalo úložiště -> INTERNAL", ah.index);
    else if (w.kind == KIVFS_DBB_W_INVALID)
        kivfs_mod_log(m, "DB_APPLY: index %" PRIu64 " vadný obsah -> MALFORMED (archivováno)",
                      ah.index);
    reply_attr(m, from->conn, req, st, w.kind == KIVFS_DBB_W_MKNODE ? &a : NULL);
}

static void on_log_head(struct kivfs_mod *m, struct kivfs_dbb *b,
                        const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    struct kivfs_dbb_logrec head;
    struct kivfs_packet resp;

    kivfs_pkt_reader_init(&rd, req);
    if (!kivfs_pkt_reader_at_end(&rd)) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_MALFORMED);
        return;
    }
    if (b->ops->log_head(b, &head) != KIVFS_STATUS_OK) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }
    if (kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(m), KIVFS_STATUS_OK) != KIVFS_OK)
        return;
    if (kivfs_pkt_add_u64(&resp, head.index) != KIVFS_OK) {
        kivfs_pkt_free(&resp);
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }
    kivfs_mod_reply(m, from->conn, &resp);
}

struct log_ctx {
    struct kivfs_packet *resp;
    uint32_t             n;
    int                  err;
};

/* Archivovaná operace [arch_hdr][DB operace] na konec stránky; když by se
 * nevešla, stránka končí (další přijde v dalším LOG_READ). */
static int emit_logrec(void *ud, const struct kivfs_dbb_logrec *r)
{
    struct log_ctx *c = ud;
    uint64_t need = (uint64_t)KIVFS_RECORD_HDR_SIZE + KIVFS_ARCH_HDR_WIRE_SIZE + r->dbop_len;
    if (c->resp->header.data_size + need > KIVFS_MAX_PAYLOAD)
        return 1;
    struct kivfs_arch_hdr ah = { r->index, r->t, r->op };
    struct kivfs_pkt_reader op_rd = { r->dbop, r->dbop_len, 0 };
    if (kivfs_arch_hdr_add(c->resp, &ah) != KIVFS_OK
        || kivfs_dbop_copy(c->resp, &op_rd) != KIVFS_OK) {
        c->err = 1;
        return 1;
    }
    c->n++;
    return 0;
}

static void on_log_read(struct kivfs_mod *m, struct kivfs_dbb *b,
                        const struct kivfs_mod_peer *from, struct kivfs_packet *req)
{
    struct kivfs_pkt_reader rd;
    struct kivfs_packet resp;
    uint64_t from_index;
    uint32_t max;

    kivfs_pkt_reader_init(&rd, req);
    if (kivfs_pkt_read_u64(&rd, &from_index) != KIVFS_OK
        || kivfs_pkt_read_u32(&rd, &max) != KIVFS_OK
        || !kivfs_pkt_reader_at_end(&rd) || from_index == 0) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_MALFORMED);
        return;
    }
    if (max > KIVFS_SYNC_FETCH_MAX)
        max = KIVFS_SYNC_FETCH_MAX;
    if (kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(m), KIVFS_STATUS_OK) != KIVFS_OK)
        return;
    struct log_ctx c = { &resp, 0, 0 };
    int16_t st = b->ops->log_read(b, from_index, max, emit_logrec, &c);
    if (st != KIVFS_STATUS_OK || c.err) {
        kivfs_pkt_free(&resp);
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return;
    }
    kivfs_mod_reply(m, from->conn, &resp);
}

/* ------------------------------------------------------------------ */

static int on_request(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                      struct kivfs_packet *req)
{
    struct kivfs_dbb *b = kivfs_mod_user(m);
    uint16_t code = req->header.request_code;
    uint32_t allowed;

    switch (code) {
    case KIVFS_REQ_DB_GET:
    case KIVFS_REQ_DB_LIST:
    case KIVFS_REQ_DB_MKNODE:
    case KIVFS_REQ_DB_RMNODE:   allowed = KIVFS_MOD_VFS;  break;
    case KIVFS_REQ_DB_APPLY:
    case KIVFS_REQ_DB_LOG_READ:
    case KIVFS_REQ_DB_LOG_HEAD: allowed = KIVFS_MOD_SYNC; break;
    default:                    return 0;
    }
    if (from->module != allowed) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_ACCESS_DENIED);
        return 1;
    }

    switch (code) {
    case KIVFS_REQ_DB_GET:
    case KIVFS_REQ_DB_LIST:     on_read(m, b, from, req);     break;
    case KIVFS_REQ_DB_MKNODE:
    case KIVFS_REQ_DB_RMNODE:   on_write(m, from, req);       break;
    case KIVFS_REQ_DB_APPLY:    on_apply(m, b, from, req);    break;
    case KIVFS_REQ_DB_LOG_READ: on_log_read(m, b, from, req); break;
    default:                    on_log_head(m, b, from, req); break;
    }
    return 1;
}

int main(int argc, char **argv)
{
    static const char *const extra_keys[] = { "backend", NULL };
    static const struct kivfs_mod_ops ops = {
        .self       = KIVFS_MOD_DB,
        .extra_keys = extra_keys,
        .init       = db_init,
        .fini       = db_fini,
        .on_request = on_request,
    };
    return kivfs_module_main(argc, argv, &ops);
}
