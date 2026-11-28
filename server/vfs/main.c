/*
 * kivfs-vfs – modul VFS (kap. 7.5): logika souborového systému a práva.
 *
 * Operace klienta přicházejí od Syncu jako [ctx][záznamy klienta].
 * VFS zkontroluje formát a cestu, zeptá se DB na atributy, ověří práva
 * (vfs/perm.h) a provede operaci v DB:
 *
 *   STAT   DB_GET cíle                         -> [attr]
 *   LIST   DB_GET cíle (r), DB_LIST            -> [entry]...
 *   MKDIR  DB_GET rodiče (w+x), DB_MKNODE      -> jen stav
 *   RMDIR  DB_GET rodiče (w+x), DB_RMNODE      -> jen stav
 *
 * Mezi kontrolou práv a zápisem může strom změnit jiná operace – DB
 * proto konzistenci kontroluje znovu (EXISTS, NOT_FOUND, NOT_EMPTY).
 * Zápisy (DB_MKNODE, DB_RMNODE) jdou v DB přes Sync (kolo Tdb, replikace
 * na všechny uzly); když na ně nepřijde odpověď, je výsledek neznámý ->
 * TIMEOUT (ne UNAVAILABLE).
 * Operace přes dvě volání DB = dvě půlky spojené pokračováním
 * (kivfs_mod_call), stav operace je v struct vop.
 * Použití: kivfs-vfs -c <soubor.ini>
 */
#include "core/module.h"
#include "core/modconf.h"
#include "proto/errors.h"
#include "proto/fsproto.h"
#include "proto/protocol.h"
#include "vfs/perm.h"

#include <stdlib.h>
#include <string.h>

/* Timeouty volání DB (kivfs_modconf_timeouts): čtení vfs_db_read, zápis
 * vfs_db_write. Zápis bez odpovědi = výsledek NEZNÁMÝ (kolo Tdb mohlo
 * proběhnout) -> TIMEOUT; čtení nic nemění -> UNAVAILABLE. */
static int db_read_ms(const struct kivfs_mod *m)
{
    struct kivfs_timeouts t;
    kivfs_modconf_timeouts(kivfs_mod_conf(m), &t);
    return t.vfs_db_read;
}

static int db_write_ms(const struct kivfs_mod *m)
{
    struct kivfs_timeouts t;
    kivfs_modconf_timeouts(kivfs_mod_conf(m), &t);
    return t.vfs_db_write;
}

/* Rozpracovaná operace (mezi 1. a 2. voláním DB). */
struct vop {
    kivfs_conn_id       to;                     /* Sync, kterému se odpoví */
    struct kivfs_header orig;                   /* hlavička požadavku (kód + ref) */
    struct kivfs_ctx    ctx;
    uint32_t            mode;                   /* MKDIR */
    char                path[KIVFS_PATH_MAX + 1];
};

static void vop_reply(struct kivfs_mod *m, struct vop *op, int16_t status)
{
    struct kivfs_packet orig = { .header = op->orig };
    kivfs_mod_reply_status(m, op->to, &orig, status);
}

static int16_t call_error_status(int rc)
{
    return rc == KIVFS_ENOTREADY ? KIVFS_STATUS_UNAVAILABLE
         : rc == KIVFS_EAGAIN    ? KIVFS_STATUS_BUSY
                                 : KIVFS_STATUS_INTERNAL;
}

/* Paket DB operace s cestou (mknode != 0 -> přidá i [mknode]). */
static int db_packet(struct kivfs_packet *q, uint16_t code, const char *path,
                     const struct vop *op, int mknode)
{
    kivfs_pkt_init(q, code, 0);
    int rc = kivfs_pkt_add_bytes(q, path, (uint32_t)strlen(path));
    if (rc == KIVFS_OK && mknode)
        rc = kivfs_mknode_add(q, KIVFS_MODE_DIR | (op->mode & KIVFS_MODE_PERM_MASK),
                              op->ctx.uid, op->ctx.gid);
    if (rc != KIVFS_OK)
        kivfs_pkt_free(q);
    return rc;
}

/* Poslední krok: přeposlat do DB a odpověď DB vrátit Syncu (STAT, LIST,
 * RMDIR). RMDIR je zápis -> jiný timeout a bez odpovědi TIMEOUT. */
static void finish_forward(struct kivfs_mod *m, struct vop *op, uint16_t db_code)
{
    struct kivfs_packet q;
    struct kivfs_packet orig = { .header = op->orig };
    struct kivfs_mod_peer to = { .conn = op->to };
    int write = db_code == KIVFS_REQ_DB_RMNODE;
    if (db_packet(&q, db_code, op->path, op, 0) != KIVFS_OK)
        vop_reply(m, op, KIVFS_STATUS_INTERNAL);
    else
        kivfs_mod_forward(m, &to, &orig, &q, KIVFS_MOD_DB,
                          write ? db_write_ms(m) : db_read_ms(m),
                          write ? KIVFS_STATUS_TIMEOUT : KIVFS_STATUS_UNAVAILABLE);
    free(op);
}

/* MKDIR, 2. půlka: DB vrací [attr], klient chce jen stav. Bez odpovědi
 * = výsledek neznámý (zápis mohl proběhnout) -> TIMEOUT. */
static void after_mknode(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct vop *op = state;
    vop_reply(m, op, err == KIVFS_OK ? resp->header.return_value : KIVFS_STATUS_TIMEOUT);
    free(op);
}

/* 1. půlka všech operací kromě STAT: atributy rodiče (MKDIR, RMDIR)
 * nebo cíle (LIST) -> kontrola práv -> 2. volání DB. */
static void after_get(struct kivfs_mod *m, void *state, int err, struct kivfs_packet *resp)
{
    struct vop *op = state;
    uint16_t code = op->orig.request_code;
    struct kivfs_attr a;
    struct kivfs_pkt_reader rd;
    uint64_t ref;

    if (err != KIVFS_OK) {
        vop_reply(m, op, KIVFS_STATUS_UNAVAILABLE);
        free(op);
        return;
    }
    if (resp->header.return_value != KIVFS_STATUS_OK) {
        vop_reply(m, op, resp->header.return_value);   /* NOT_FOUND, NOT_DIR, ... */
        free(op);
        return;
    }
    kivfs_pkt_reader_init(&rd, resp);
    if (kivfs_pkt_read_u64(&rd, &ref) != KIVFS_OK || kivfs_attr_read(&rd, &a) != KIVFS_OK) {
        vop_reply(m, op, KIVFS_STATUS_INTERNAL);
        free(op);
        return;
    }

    unsigned need = code == KIVFS_REQ_LIST ? KIVFS_PERM_R : (KIVFS_PERM_W | KIVFS_PERM_X);
    int16_t st = kivfs_perm_check_dir(&a, &op->ctx, need);
    if (st != KIVFS_STATUS_OK) {
        vop_reply(m, op, st);
        free(op);
        return;
    }

    if (code == KIVFS_REQ_LIST) {
        finish_forward(m, op, KIVFS_REQ_DB_LIST);
    } else if (code == KIVFS_REQ_RMDIR) {
        finish_forward(m, op, KIVFS_REQ_DB_RMNODE);
    } else {                                           /* MKDIR */
        struct kivfs_packet q;
        int rc = db_packet(&q, KIVFS_REQ_DB_MKNODE, op->path, op, 1);
        if (rc == KIVFS_OK)
            rc = kivfs_mod_call(m, KIVFS_MOD_DB, &q, db_write_ms(m), after_mknode, op);
        if (rc != KIVFS_OK) {
            vop_reply(m, op, call_error_status(rc));
            free(op);
        }
    }
}

static int on_request(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                      struct kivfs_packet *req)
{
    uint16_t code = req->header.request_code;
    if (from->module != KIVFS_MOD_SYNC || code < KIVFS_REQ_STAT || code > KIVFS_REQ_RMDIR)
        return 0;

    struct vop *op = calloc(1, sizeof *op);
    if (op == NULL) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_INTERNAL);
        return 1;
    }
    op->to   = from->conn;
    op->orig = req->header;

    /* [ctx][path] (+ [u32 mode] u MKDIR), nic navíc */
    struct kivfs_pkt_reader rd;
    kivfs_pkt_reader_init(&rd, req);
    if (kivfs_ctx_read(&rd, &op->ctx) != KIVFS_OK
        || kivfs_path_read(&rd, op->path) != KIVFS_OK
        || (code == KIVFS_REQ_MKDIR && kivfs_pkt_read_u32(&rd, &op->mode) != KIVFS_OK)
        || !kivfs_pkt_reader_at_end(&rd)) {
        vop_reply(m, op, KIVFS_STATUS_MALFORMED);
        free(op);
        return 1;
    }

    int is_root = strcmp(op->path, "/") == 0;
    if (code == KIVFS_REQ_MKDIR && is_root) {
        vop_reply(m, op, KIVFS_STATUS_EXISTS);
        free(op);
        return 1;
    }
    if (code == KIVFS_REQ_RMDIR && is_root) {
        vop_reply(m, op, KIVFS_STATUS_ACCESS_DENIED);
        free(op);
        return 1;
    }

    if (code == KIVFS_REQ_STAT) {                      /* bez kontroly práv */
        finish_forward(m, op, KIVFS_REQ_DB_GET);
        return 1;
    }

    /* LIST: atributy cíle; MKDIR/RMDIR: atributy rodiče */
    char target[KIVFS_PATH_MAX + 1];
    const char *name;
    if (code == KIVFS_REQ_LIST)
        strcpy(target, op->path);
    else
        kivfs_path_split(op->path, target, &name);     /* "/" je vyloučené výše */

    struct kivfs_packet q;
    int rc = db_packet(&q, KIVFS_REQ_DB_GET, target, op, 0);
    if (rc == KIVFS_OK)
        rc = kivfs_mod_call(m, KIVFS_MOD_DB, &q, db_read_ms(m), after_get, op);
    if (rc != KIVFS_OK) {
        vop_reply(m, op, call_error_status(rc));
        free(op);
    }
    return 1;
}

int main(int argc, char **argv)
{
    static const struct kivfs_mod_ops ops = {
        .self = KIVFS_MOD_VFS,
        .on_request = on_request,
    };
    return kivfs_module_main(argc, argv, &ops);
}
