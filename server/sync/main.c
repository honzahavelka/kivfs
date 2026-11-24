/*
 * kivfs-sync – modul SYNC (kap. 7.4): synchronizace požadavků mezi uzly.
 *
 * Zatím: operace klienta od Auth jen přepošle do VFS (beze změny, bez
 * kopírování payloadu). Sem později přijde řazení podle Lamportových
 * hodin a dohoda s ostatními uzly – přeposílání se nahradí "zařaď
 * a dohodni se".
 * Použití: kivfs-sync -c <soubor.ini>
 */
#include "core/module.h"
#include "proto/protocol.h"

#define VFS_TIMEOUT_MS 3000

static int on_request(struct kivfs_mod *m, const struct kivfs_mod_peer *from,
                      struct kivfs_packet *req)
{
    uint16_t code = req->header.request_code;
    if (from->module != KIVFS_MOD_AUTH || code < KIVFS_REQ_STAT || code > KIVFS_REQ_RMDIR)
        return 0;

    /* payload převezmeme (bez kopie); hlavička req zůstane kvůli ref */
    struct kivfs_packet fwd = *req;
    req->data = NULL;
    req->capacity = 0;
    kivfs_mod_forward(m, from, req, &fwd, KIVFS_MOD_VFS, VFS_TIMEOUT_MS);
    return 1;
}

int main(int argc, char **argv)
{
    static const struct kivfs_mod_ops ops = {
        .self = KIVFS_MOD_SYNC,
        .on_request = on_request,
    };
    return kivfs_module_main(argc, argv, &ops);
}
