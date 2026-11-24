#ifndef KIVFS_VFS_PERM_H
#define KIVFS_VFS_PERM_H

/*
 * KIVFS – kontrola přístupových práv ve VFS (zjednodušený POSIX)
 * ===============================================================
 *
 * Čisté funkce bez sítě a bez stavu – testovatelné samostatně.
 *
 *   - uid 0 smí všechno
 *   - vlastník (uid) -> bity rwx------, skupina (gid) -> ---rwx---,
 *     ostatní -> ------rwx
 *   - kontroluje se jen bezprostřední uzel (rodič nebo cíl), předci ne
 *
 * Co která operace potřebuje (rozhodnuto 24. 11.):
 *   STAT   nic
 *   LIST   r na cílovém adresáři
 *   MKDIR  w+x na rodiči
 *   RMDIR  w+x na rodiči
 */

#include "proto/fsproto.h"

#define KIVFS_PERM_R 4u
#define KIVFS_PERM_W 2u
#define KIVFS_PERM_X 1u

/* 1, pokud má "who" na uzlu "a" všechna práva z "need" (kombinace R|W|X). */
int kivfs_perm_allowed(const struct kivfs_attr *a, const struct kivfs_ctx *who, unsigned need);

/* Kontrola adresáře, do kterého se zasahuje (rodič u MKDIR/RMDIR, cíl
 * u LIST): vrací KIVFS_STATUS_OK, NOT_DIR (není adresář) nebo
 * ACCESS_DENIED (chybí práva "need"). */
int16_t kivfs_perm_check_dir(const struct kivfs_attr *dir, const struct kivfs_ctx *who,
                             unsigned need);

#endif /* KIVFS_VFS_PERM_H */
