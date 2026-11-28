#ifndef KIVFS_FSPROTO_H
#define KIVFS_FSPROTO_H

/*
 * KIVFS – datové struktury operací souborového systému na drátě
 * =============================================================
 *
 * Pravidlo pro úsporu bajtů: každý záznam nese 4B délku, takže pevné
 * struktury se posílají jako JEDEN binární záznam s pevným rozložením
 * (big-endian, ručně jako hlavička), ne jako N záznamů po jednom čísle.
 * Samostatné záznamy jen pro data proměnné délky (cesty, jména).
 * Příklad: attr = 4 + 44 = 48 B místo 7 záznamů = 72 B.
 *
 * Rozšiřitelnost: nové pole se přidá NA KONEC záznamu; čtenář kontroluje
 * jen délku >= minimum a přebytečné bajty ignoruje.
 *
 *   attr   (44 B)  u64 id | u32 mode | u32 uid | u32 gid
 *                  | u64 size | u64 mtime_ms | u64 ctime_ms
 *   ctx    (8 B)   u32 uid | u32 gid              (Auth -> Sync -> VFS)
 *   entry  (12 B + jméno)  u64 id | u32 mode | jméno (délka = zbytek záznamu)
 *   mknode (12 B)  u32 mode | u32 uid | u32 gid   (VFS -> DB)
 *
 * mode: typ v horních bitech (jako st_mode), práva v dolních 12 bitech.
 * Časy: milisekundy od epochy (UTC).
 */

#include <stddef.h>
#include <stdint.h>
#include "proto/packet.h"

#define KIVFS_NAME_MAX         255     /* bajtů jedné složky cesty */
#define KIVFS_PATH_MAX         4096    /* bajtů celé cesty (bez '\0') */

#define KIVFS_ATTR_WIRE_SIZE   44u
#define KIVFS_CTX_WIRE_SIZE    8u
#define KIVFS_ENTRY_FIXED_SIZE 12u
#define KIVFS_MKNODE_WIRE_SIZE 12u

#define KIVFS_MODE_TYPE_MASK   0170000u
#define KIVFS_MODE_DIR         0040000u
#define KIVFS_MODE_FILE        0100000u
#define KIVFS_MODE_PERM_MASK   0007777u

/* Velikost bloku (fragmentu) souboru, kap. 7.7.2: [fs] fragment.
 * Horní mez: blok + hlavička + lístek + otisk se musí vejít do paketu
 * (KIVFS_MAX_PAYLOAD 16 MiB). Velikost se ukládá u každého souboru,
 * proto úložiště přijme cokoli do MAX bez ohledu na konfiguraci. */
#define KIVFS_FS_BLOCK_MIN     (64u * 1024u)
#define KIVFS_FS_BLOCK_MAX     (15u * 1024u * 1024u)
#define KIVFS_FS_BLOCK_DEFAULT (4u * 1024u * 1024u)

struct kivfs_attr {
    uint64_t id;
    uint32_t mode, uid, gid;
    uint64_t size;
    uint64_t mtime_ms, ctime_ms;
};

struct kivfs_ctx {
    uint32_t uid, gid;
};

/* Přidání na konec paketu: KIVFS_OK / KIVFS_ENOMEM / KIVFS_ETOOBIG
 * (u entry navíc KIVFS_EINVAL pro jméno delší než KIVFS_NAME_MAX). */
int kivfs_attr_add  (struct kivfs_packet *p, const struct kivfs_attr *a);
int kivfs_ctx_add   (struct kivfs_packet *p, const struct kivfs_ctx *c);
int kivfs_mknode_add(struct kivfs_packet *p, uint32_t mode, uint32_t uid, uint32_t gid);
/* name nemusí být zakončené '\0'; name_len <= KIVFS_NAME_MAX */
int kivfs_entry_add (struct kivfs_packet *p, uint64_t id, uint32_t mode,
                     const char *name, size_t name_len);

/* Čtení dalšího záznamu: KIVFS_OK, KIVFS_EEND, KIVFS_EMALFORMED (krátký
 * záznam). Při chybě se reader neposune. */
int kivfs_attr_read  (struct kivfs_pkt_reader *r, struct kivfs_attr *a);
int kivfs_ctx_read   (struct kivfs_pkt_reader *r, struct kivfs_ctx *c);
int kivfs_mknode_read(struct kivfs_pkt_reader *r, uint32_t *mode, uint32_t *uid, uint32_t *gid);
/* *name ukazuje do paketu (bez '\0'), platí do kivfs_pkt_free */
int kivfs_entry_read (struct kivfs_pkt_reader *r, uint64_t *id, uint32_t *mode,
                      const char **name, uint32_t *name_len);

/* Přečte záznam s cestou do dst (KIVFS_PATH_MAX + 1 B) a ověří pravidla:
 *   - začíná '/', žádné prázdné složky ("//"), žádné "." ani "..",
 *   - bez '/' na konci (kromě samotného "/"),
 *   - složka <= KIVFS_NAME_MAX, celá cesta <= KIVFS_PATH_MAX, žádný bajt 0.
 * KIVFS_OK, KIVFS_EEND, KIVFS_EMALFORMED (pravidla nesplněna). */
int kivfs_path_read(struct kivfs_pkt_reader *r, char *dst);

/* Rozdělí platnou cestu na rodiče a poslední složku:
 *   "/a/b" -> parent "/a", name "b";  "/a" -> parent "/", name "a".
 * parent_dst má KIVFS_PATH_MAX + 1 B, name ukazuje do path.
 * Pro "/" vrací KIVFS_EINVAL (nemá rodiče). */
int kivfs_path_split(const char *path, char *parent_dst, const char **name);

#endif /* KIVFS_FSPROTO_H */
