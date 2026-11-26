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
 * Vztahy mezi moduly – DVĚ relace:
 *
 * 1) kivfs_module_may_call(caller, callee) – kdo smí založit spojení
 *    a posílat požadavky. Kostra z ní odvozuje odchozí linky (reconnect,
 *    kivfs_mod_call), povinné sekce konfigurace, kontrolu HELLO
 *    a připravenost modulu (diag ready = všechny linky na moduly
 *    stejného uzlu jsou UP).
 *      CLIENT -> AUTH          (klient smí JEN do Auth)
 *      AUTH   -> SYNC          (stejný uzel)
 *      SYNC   -> VFS           (stejný uzel; klientské operace)
 *      SYNC   -> DB            (stejný uzel; DB_APPLY, DB_LOG_READ/HEAD)
 *      SYNC   -> SYNC          (jiný uzel z [cluster])
 *      VFS    -> DB, FS        (stejný uzel)
 *      DB     -> SYNC          (stejný uzel; SYNC_SUBMIT)
 *    Relace NENÍ acyklická (SYNC <-> DB) – nepoužívat ji k ničemu, co
 *    chodí rekurzivně po modulech.
 *    Mezi SYNC a DB jsou DVĚ nezávislá spojení (směr spojení je pevný):
 *    SUBMIT jde po lince DB->SYNC, APPLY po lince SYNC->DB. Výpadek
 *    jedné neruší druhou.
 *
 * 2) kivfs_module_status_child(parent, child) – strom kaskády STATUS:
 *      AUTH -> SYNC -> VFS -> DB, FS
 *    Podmnožina relace 1 (jen moduly stejného uzlu, bez SYNC->SYNC),
 *    každý serverový modul kromě AUTH má právě jednoho rodiče. Kaskáda
 *    jde jen po hranách stromu → je konečná a každý modul uzlu je
 *    v odpovědi právě jednou. Hloubka stromu určuje timeouty STATUS.
 *
 * Kontrolu uzlů (stejný / jiný / známý) dělá modul – potřebuje konfiguraci.
 *
 * Tady jsou jen čisté funkce bez sítě – používá je server i klient.
 */

#include <stddef.h>
#include <stdint.h>
#include "proto/packet.h"

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

/* 1, pokud modul caller smí založit spojení na modul callee (relace 1). */
int  kivfs_module_may_call(uint32_t caller, uint32_t callee);

/* 1, pokud se modul parent při STATUS ptá modulu child (relace 2). */
int  kivfs_module_status_child(uint32_t parent, uint32_t child);

/* Otisk seznamu uzlů: FNV-1a (32 bit) přes počet a id seřazená VZESTUPNĚ,
 * každé jako 4 B big-endian. Hashují se jen id, ne adresy – stejný uzel
 * může mít na různých strojích zapsanou IP i hostname. Nikdy nevrací 0
 * (0 = "nekontrolovat", posílá klient). */
uint32_t kivfs_cluster_hash(const uint32_t *ids_sorted, size_t n);

/* Nová inkarnace: 8 náhodných bajtů z /dev/urandom (záloha: čas ^ pid).
 * Nikdy nevrací 0. */
uint64_t kivfs_incarnation_new(void);

#endif /* KIVFS_HELLO_H */
