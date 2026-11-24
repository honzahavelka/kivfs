#ifndef KIVFS_DIAG_H
#define KIVFS_DIAG_H

/*
 * KIVFS – diagnostika propojení (blok modulu v odpovědi STATUS)
 * =============================================================
 *
 * Odpověď na KIVFS_REQ_STATUS = [u64 ref] + 1..N bloků (kaskáda Auth ->
 * Sync -> VFS -> DB, FS, viz protocol.h). Jeden blok = jeden modul:
 *
 *   [u32 node][u32 module][u64 incarnation][u32 ready]
 *   [u32 n_out]  n_out × ( [u32 module][u32 node][u32 state][u64 peer_incarnation] )
 *   [u32 n_in]   n_in  × ( [u32 module][u32 node] )
 *
 *   ready   1 = všechny moduly, které volám (bez Synců ostatních uzlů), jsou UP
 *   out     moje odchozí linky (kam se připojuji)
 *   in      představená příchozí spojení (kdo je připojený ke mně)
 * Stejné spojení tak hlásí obě strany – kivfs-check je porovná.
 */

#include <stdint.h>
#include "packet.h"

#define KIVFS_DIAG_MAX_LINKS 64

enum kivfs_link_state {
    KIVFS_LINK_DOWN       = 0,   /* nepřipojeno, čeká se na další pokus */
    KIVFS_LINK_CONNECTING = 1,   /* connect nebo HELLO probíhá */
    KIVFS_LINK_UP         = 2    /* HELLO prošlo */
};

struct kivfs_diag_link {
    uint32_t module, node, state;
    uint64_t peer_incarnation;   /* 0, pokud linka není UP */
};

struct kivfs_diag_peer {
    uint32_t module, node;
};

struct kivfs_diag_module {
    uint32_t node, module;
    uint64_t incarnation;
    uint32_t ready;
    uint32_t n_out;
    struct kivfs_diag_link out[KIVFS_DIAG_MAX_LINKS];
    uint32_t n_in;
    struct kivfs_diag_peer in[KIVFS_DIAG_MAX_LINKS];
};

/* Připojí blok na konec paketu (n_out/n_in nad maximum se ořízne).
 * KIVFS_OK, KIVFS_ENOMEM, KIVFS_ETOOBIG. Při chybě může paket obsahovat
 * část bloku – volající ho pak zahodí. */
int  kivfs_diag_add(struct kivfs_packet *p, const struct kivfs_diag_module *m);

/* Přečte další blok. KIVFS_EEND = žádný další, KIVFS_EMALFORMED. */
int  kivfs_diag_read(struct kivfs_pkt_reader *r, struct kivfs_diag_module *m);

const char *kivfs_link_state_str(uint32_t state);

#endif /* KIVFS_DIAG_H */
