/*
 * kivfs-auth – modul AUTH (kap. 7.3): vstupní brána, klient smí jen sem.
 *
 * Zatím: autentizace je MOCK – každý klient je uid/gid 1000. Operace
 * klienta (STAT, MKDIR, LIST, RMDIR) Auth přepošle do Syncu se stejným
 * kódem a před záznamy klienta vloží [ctx] (uid, gid); odpověď vrátí
 * klientovi.
 * Použití: kivfs-auth -c <soubor.ini>
 */
#include "core/module.h"
#include "proto/errors.h"
#include "proto/fsproto.h"
#include "proto/protocol.h"

#define MOCK_UID           1000u
#define MOCK_GID           1000u

static int is_client_op(uint16_t code)
{
    return code >= KIVFS_REQ_STAT && code <= KIVFS_REQ_RMDIR;
}

static int on_request(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                      struct kivfs_packet *req)
{
    if (!is_client_op(req->header.request_code))
        return 0;

    struct kivfs_ctx ctx = { MOCK_UID, MOCK_GID };
    struct kivfs_packet fwd;
    struct kivfs_pkt_reader rd;
    const uint8_t *b;
    uint32_t len;
    int rc;

    /* [ctx] + záznamy klienta beze změny (obsah kontroluje VFS) */
    kivfs_pkt_init(&fwd, req->header.request_code, 0);
    rc = kivfs_ctx_add(&fwd, &ctx);
    kivfs_pkt_reader_init(&rd, req);
    while (rc == KIVFS_OK && (rc = kivfs_pkt_read_bytes(&rd, &b, &len)) == KIVFS_OK)
        rc = kivfs_pkt_add_bytes(&fwd, b, len);
    if (rc != KIVFS_EEND) {
        kivfs_pkt_free(&fwd);
        kivfs_mod_reply_status(m, from->conn, req,
                               rc == KIVFS_EMALFORMED ? KIVFS_STATUS_MALFORMED
                                                      : KIVFS_STATUS_INTERNAL);
        return 1;
    }
    /* Auth -> Sync = celý časový rámec operace ([cluster] op_timeout_ms);
     * Sync si své kroky odvodí kratší (kivfs_modconf_timeouts), ať odpověď
     * stihne zpět. Bez odpovědi = výsledek neznámý (zápis mohl proběhnout)
     * -> TIMEOUT; Auth čtení a zápis nerozlišuje, u čtení TIMEOUT neškodí. */
    struct kivfs_timeouts to;
    kivfs_modconf_timeouts(kivfs_mod_conf(m), &to);
    kivfs_mod_forward(m, from, req, &fwd, KIVFS_MOD_SYNC, to.op, KIVFS_STATUS_TIMEOUT);
    return 1;
}

int main(int argc, char **argv)
{
    static const struct kivfs_mod_ops ops = {
        .self = KIVFS_MOD_AUTH,
        .on_request = on_request,
    };
    return kivfs_module_main(argc, argv, &ops);
}
