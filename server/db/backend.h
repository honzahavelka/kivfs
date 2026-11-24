#ifndef KIVFS_DB_BACKEND_H
#define KIVFS_DB_BACKEND_H

/*
 * KIVFS – úložiště metadat modulu DB
 * ==================================
 *
 * Modul DB převádí požadavky KIVFS_REQ_DB_* na volání backendu. Backend
 * je vyměnitelný: teď kivfs_dbb_mock (strom v paměti, při restartu se
 * ztratí), později MySQL za stejným rozhraním.
 *
 * Volání jsou SYNCHRONNÍ – worker DB smí blokovat na databázi (síť ne).
 * Volá je jen worker DB, backend proto nemusí být thread-safe.
 *
 * Výsledek je přímo stav na drátě (enum kivfs_status), protože backend
 * zná sémantiku chyby (NOT_FOUND vs. NOT_DIR vs. EXISTS); INTERNAL =
 * selhání úložiště (detail backend zaloguje).
 *
 * Konzistenci stromu hlídá backend (rodič existuje a je adresář, jméno
 * je volné, mazaný adresář je prázdný) – VFS se na ni spoléhat musí,
 * protože mezi jeho kontrolou práv a zápisem může strom změnit jiná
 * operace. Práva backend NEkontroluje (to dělá VFS).
 *
 * Cesty dostává už zkontrolované (kivfs_path_read).
 * Kořen "/" existuje vždy: id 1, KIVFS_MODE_DIR | 0777, uid 0, gid 0.
 */

#include <stdint.h>
#include "proto/fsproto.h"

struct kivfs_dbb;

struct kivfs_dbb_ops {
    /* atributy uzlu; NOT_FOUND, NOT_DIR (složka cesty je soubor) */
    int16_t (*get)(struct kivfs_dbb *b, const char *path, struct kivfs_attr *out);

    /* nový uzel; mode včetně typu; časy = now_ms; id přidělí backend
     * NOT_FOUND (rodič), NOT_DIR (rodič není adresář), EXISTS */
    int16_t (*mknode)(struct kivfs_dbb *b, const char *path, uint32_t mode,
                      uint32_t uid, uint32_t gid, uint64_t now_ms, struct kivfs_attr *out);

    /* položky adresáře seřazené podle jména; emit vrací 0 = pokračovat,
     * jinak průchod skončí a list vrátí INTERNAL (např. přetekl paket)
     * NOT_FOUND, NOT_DIR */
    int16_t (*list)(struct kivfs_dbb *b, const char *path,
                    int (*emit)(void *ud, uint64_t id, uint32_t mode, const char *name),
                    void *ud);

    /* smazání; NOT_FOUND, NOT_EMPTY, ACCESS_DENIED ("/"), NOT_DIR
     * (zatím jen adresáře – rmdir) */
    int16_t (*rmnode)(struct kivfs_dbb *b, const char *path);

    void (*close)(struct kivfs_dbb *b);
};

/* Instance backendu = ops + jeho data. */
struct kivfs_dbb {
    const struct kivfs_dbb_ops *ops;
};

/* Mock: strom v paměti. KIVFS_OK / KIVFS_ENOMEM. */
int kivfs_dbb_mock_open(struct kivfs_dbb **out);

#endif /* KIVFS_DB_BACKEND_H */
