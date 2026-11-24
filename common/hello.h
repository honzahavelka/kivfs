#ifndef KIVFS_HELLO_H
#define KIVFS_HELLO_H

/*
 * KIVFS – HELLO: představení stran na začátku spojení
 * ===================================================
 *
 * Formát na drátě (protocol.h, KIVFS_REQ_HELLO):
 *   požadavek: [u32 node_id][u32 module][u32 proto_version]
 *              [u64 incarnation][u32 cluster_hash]
 *   odpověď:   [u64 ref] + stejných 5 polí za odpovídající stranu
 * Všechna pole jsou u32/u64 (paketové API nemá menší typy; pár bajtů na
 * spojení nevadí).
 *
 *   node_id        uzel odesílatele (>= 1), klient 0
 *   module         enum kivfs_module odesílatele
 *   proto_version  KIVFS_PROTO_VERSION; jiná verze = odmítnout
 *   incarnation    náhodné číslo vygenerované při startu procesu. Podle
 *                  něj protistrana pozná restart (jiná inkarnace = nový
 *                  proces, jeho rozpracovaný stav zmizel) a modul pozná,
 *                  že se připojil sám na sebe (dostal vlastní inkarnaci).
 *   cluster_hash   otisk seznamu uzlů z [cluster]; jiný otisk = uzly mají
 *                  různou konfiguraci. Klient posílá 0 (nekontroluje se).
 *
 * Kdo se smí s kým spojit (kivfs_module_may_call):
 *   CLIENT -> AUTH          (klient smí JEN do Auth)
 *   AUTH   -> SYNC          (stejný uzel)
 *   SYNC   -> VFS           (stejný uzel)
 *   SYNC   -> SYNC          (jiný uzel z [cluster])
 *   VFS    -> DB, FS        (stejný uzel)
 * Kontrolu uzlů (stejný / jiný / známý) dělá modul – potřebuje konfiguraci.
 *
 * Tady jsou jen čisté funkce bez sítě – používá je server i klient.
 */

#include <stddef.h>
#include <stdint.h>
#include "packet.h"

struct kivfs_hello {
    uint32_t node_id;
    uint32_t module;          /* enum kivfs_module */
    uint32_t version;         /* KIVFS_PROTO_VERSION */
    uint64_t incarnation;
    uint32_t cluster_hash;
};

/* Sestaví požadavek HELLO s timestampem ts. KIVFS_OK / KIVFS_ENOMEM
 * (pak je req prázdný). */
int  kivfs_hello_request(struct kivfs_packet *req, uint64_t ts, const struct kivfs_hello *me);

/* Sestaví odpověď na req se stavem status a s mými údaji (i při
 * REJECTED – protistrana pak v logu vidí, s kým mluvila). */
int  kivfs_hello_response(struct kivfs_packet *resp, const struct kivfs_packet *req,
                          uint64_t ts, int16_t status, const struct kivfs_hello *me);

/* Přečte pole z požadavku / z odpovědi (ref přeskočí). Ověří jen formát
 * (počet a velikost záznamů), ne obsah. KIVFS_OK / KIVFS_EMALFORMED. */
int  kivfs_hello_parse_request (const struct kivfs_packet *req,  struct kivfs_hello *out);
int  kivfs_hello_parse_response(const struct kivfs_packet *resp, struct kivfs_hello *out);

/* 1, pokud modul caller smí založit spojení na modul callee (tabulka výše). */
int  kivfs_module_may_call(uint32_t caller, uint32_t callee);

/* Otisk seznamu uzlů: FNV-1a (32 bit) přes počet a id seřazená VZESTUPNĚ,
 * každé jako 4 B big-endian. Hashují se jen id, ne adresy – stejný uzel
 * může mít na různých strojích zapsanou IP i hostname. Nikdy nevrací 0
 * (0 = "nekontrolovat", posílá klient). */
uint32_t kivfs_cluster_hash(const uint32_t *ids_sorted, size_t n);

/* Nová inkarnace: 8 náhodných bajtů z /dev/urandom (záloha: čas ^ pid).
 * Nikdy nevrací 0. */
uint64_t kivfs_incarnation_new(void);

#endif /* KIVFS_HELLO_H */
