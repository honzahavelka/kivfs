/*
 * kivfs-db – modul DB (kap. 7.6): metadata.
 *
 * Obsluhuje KIVFS_REQ_DB_* od VFS nad vyměnitelným backendem
 * (db/backend.h). Práva nekontroluje (to dělá VFS), konzistenci stromu
 * hlídá backend. Volání backendu jsou synchronní – worker DB smí
 * blokovat na databázi.
 *
 * Konfigurace, sekce [db]:
 *   backend = mock      (výchozí; strom v paměti, při restartu se ztratí)
 * Použití: kivfs-db -c <soubor.ini>
 */
#define _POSIX_C_SOURCE 200809L

#include "core/module.h"
#include "db/backend.h"
#include "proto/errors.h"
#include "proto/fsproto.h"
#include "proto/protocol.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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

/* Kontext pro list: položky se přidávají rovnou do odpovědi. */
struct list_ctx {
    struct kivfs_packet *resp;
};

static int emit_entry(void *ud, uint64_t id, uint32_t mode, const char *name)
{
    struct list_ctx *l = ud;
    return kivfs_entry_add(l->resp, id, mode, name, strlen(name)) == KIVFS_OK ? 0 : 1;
}

static int on_request(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                      struct kivfs_packet *req)
{
    uint16_t code = req->header.request_code;
    if (code < KIVFS_REQ_DB_GET || code > KIVFS_REQ_DB_RMNODE)
        return 0;

    struct kivfs_dbb *b = kivfs_mod_user(m);
    struct kivfs_pkt_reader rd;
    struct kivfs_packet resp;
    struct kivfs_attr a;
    char path[KIVFS_PATH_MAX + 1];
    uint32_t mode = 0, uid = 0, gid = 0;
    int16_t st;

    kivfs_pkt_reader_init(&rd, req);
    if (kivfs_path_read(&rd, path) != KIVFS_OK
        || (code == KIVFS_REQ_DB_MKNODE && kivfs_mknode_read(&rd, &mode, &uid, &gid) != KIVFS_OK)
        || !kivfs_pkt_reader_at_end(&rd)) {
        kivfs_mod_reply_status(m, from->conn, req, KIVFS_STATUS_MALFORMED);
        return 1;
    }

    switch (code) {
    case KIVFS_REQ_DB_GET:
    case KIVFS_REQ_DB_MKNODE:
        st = code == KIVFS_REQ_DB_GET
            ? b->ops->get(b, path, &a)
            : b->ops->mknode(b, path, mode, uid, gid, now_ms(), &a);
        if (st == KIVFS_STATUS_OK
            && kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(m), st) == KIVFS_OK) {
            if (kivfs_attr_add(&resp, &a) == KIVFS_OK) {
                kivfs_mod_reply(m, from->conn, &resp);
                return 1;
            }
            kivfs_pkt_free(&resp);
            st = KIVFS_STATUS_INTERNAL;
        }
        break;

    case KIVFS_REQ_DB_LIST:
        if (kivfs_pkt_init_response(&resp, req, kivfs_mod_next_ts(m), KIVFS_STATUS_OK) != KIVFS_OK) {
            st = KIVFS_STATUS_INTERNAL;
            break;
        }
        {
            struct list_ctx l = { &resp };
            /* emit selže, když položky přerostou KIVFS_MAX_PAYLOAD -> INTERNAL */
            st = b->ops->list(b, path, emit_entry, &l);
        }
        if (st == KIVFS_STATUS_OK) {
            kivfs_mod_reply(m, from->conn, &resp);
            return 1;
        }
        kivfs_pkt_free(&resp);
        break;

    default: /* KIVFS_REQ_DB_RMNODE */
        st = b->ops->rmnode(b, path, now_ms());
        break;
    }
    kivfs_mod_reply_status(m, from->conn, req, st);
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
