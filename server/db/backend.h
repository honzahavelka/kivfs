#ifndef KIVFS_DB_BACKEND_H
#define KIVFS_DB_BACKEND_H

/*
 * KIVFS – úložiště metadat modulu DB
 * ==================================
 *
 * Modul DB převádí požadavky na volání backendu. Backend je vyměnitelný:
 * teď kivfs_dbb_mock (strom + archiv v paměti, při restartu se ztratí),
 * později MySQL za stejným rozhraním.
 *
 * Volání jsou SYNCHRONNÍ – worker DB smí blokovat na databázi (síť ne).
 * Volá je jen worker DB, backend proto nemusí být thread-safe.
 * Výsledek je přímo stav na drátě (enum kivfs_status); detail chyby
 * úložiště backend zaloguje.
 *
 * ČTENÍ (get, list) – lokálně, na pokyn VFS.
 *
 * ZÁPISY jen přes apply() – na pokyn Syncu (DB_APPLY), na všech uzlech
 * ve stejném pořadí (index). apply provede změnu stromu A zapíše záznam
 * do archivu (MESSAGE_LOGS, kap. 7.6) ATOMICKY: MySQL jednou transakcí,
 * mock tak, že vše alokuje předem a mění až potom.
 *
 * Determinismus: výsledek apply smí záviset JEN na stavu stromu a na w
 * (žádné hodiny, náhoda, auto-increment) – id nového uzlu i čas
 * přicházejí ve w. Podmínky zápisu (rodič existuje a je adresář, jméno
 * je volné, mazaný adresář je prázdný) vyhodnocuje backend při apply,
 * takže souběžné operace dopadnou na všech uzlech stejně. Práva backend
 * NEkontroluje (VFS).
 *
 * Invariant apply (spoléhá na něj Sync):
 *   INTERNAL  nic se nezměnilo ani nezapsalo, head beze změny
 *             (selhání úložiště, log->index != head + 1)
 *   jinak     index spotřebován: záznam je v archivu i s tímto stavem,
 *             head = log->index (OK, EXISTS, NOT_FOUND, NOT_DIR,
 *             NOT_EMPTY, ACCESS_DENIED, MALFORMED…)
 *
 * Cesty dostává už zkontrolované (kivfs_path_read).
 * Kořen "/" existuje vždy: id KIVFS_DB_ROOT_ID, KIVFS_MODE_DIR | 0777,
 * uid 0, gid 0, časy 0.
 */

#include <stdint.h>
#include "proto/fsproto.h"
#include "proto/syncproto.h"

#define KIVFS_DB_ROOT_ID  1u

/* Id uzlu, který vytvoří operace s daným indexem (indexy od 1, kořen má
 * 1 -> první operace dostane 2). Jedno místo pro pravidlo; operace, které
 * by vytvářely víc uzlů, ho změní (např. index << k | i). */
static inline uint64_t kivfs_db_id_from_index(uint64_t index)
{
    return index + 1;
}

/* Záznam archivu = archivovaná operace + výsledek. */
struct kivfs_dbb_logrec {
    uint64_t           index;     /* od 1, souvisle */
    uint64_t           t;         /* T_final */
    struct kivfs_op_id op;
    int16_t            status;    /* výsledek apply (pro opakované APPLY) */
    const uint8_t     *dbop;      /* DB operace beze změny: [dbop] + záznamy
                                   * vč. hlaviček záznamů (kivfs_dbop_skip) */
    uint32_t           dbop_len;
};

enum kivfs_dbb_wkind {
    KIVFS_DBB_W_INVALID = 0,      /* vadný obsah: jen archivovat, MALFORMED */
    KIVFS_DBB_W_MKNODE  = 1,
    KIVFS_DBB_W_RMNODE  = 2
};

/* Rozbalený zápis – sestaví ho DB z DB operace. */
struct kivfs_dbb_write {
    uint32_t    kind;             /* enum kivfs_dbb_wkind */
    uint64_t    time_ms;          /* dbop.time_ms (od původce) */
    const char *path;             /* NULL u INVALID */
    uint64_t    new_id;           /* MKNODE: kivfs_db_id_from_index(index) */
    uint32_t    mode, uid, gid;   /* MKNODE: mode včetně typu */
};

struct kivfs_dbb;

struct kivfs_dbb_ops {
    /* atributy uzlu; NOT_FOUND, NOT_DIR (složka cesty je soubor) */
    int16_t (*get)(struct kivfs_dbb *b, const char *path, struct kivfs_attr *out);

    /* položky adresáře seřazené podle jména; emit vrací 0 = pokračovat,
     * jinak průchod skončí a list vrátí INTERNAL; NOT_FOUND, NOT_DIR */
    int16_t (*list)(struct kivfs_dbb *b, const char *path,
                    int (*emit)(void *ud, uint64_t id, uint32_t mode, const char *name),
                    void *ud);

    /* Provede zápis w a archivuje log (log->status se ignoruje – zapíše se
     * výsledek). Invariant viz výše. Výsledky podle w->kind:
     *   MKNODE   OK (out = atributy nového uzlu; mtime rodiče = time_ms),
     *            NOT_FOUND / NOT_DIR (rodič), EXISTS (i "/")
     *   RMNODE   OK (mtime rodiče = time_ms), NOT_FOUND, NOT_EMPTY,
     *            NOT_DIR (zatím jen adresáře), ACCESS_DENIED ("/")
     *   INVALID  MALFORMED (nic nemění, jen archivuje)
     * out smí být NULL. */
    int16_t (*apply)(struct kivfs_dbb *b, const struct kivfs_dbb_write *w,
                     const struct kivfs_dbb_logrec *log, struct kivfs_attr *out);

    /* Poslední záznam archivu (last->index = 0 a nic dalšího, když je
     * prázdný). last->dbop ukazuje do backendu – platí do dalšího volání.
     * OK / INTERNAL. */
    int16_t (*log_head)(struct kivfs_dbb *b, struct kivfs_dbb_logrec *last);

    /* Záznamy od from (from 0 = od 1) vzestupně, nejvýš max; emit vrací
     * 0 = další, jinak stop (stránka je plná) – i pak OK. Záznam platí jen
     * během emit. from > head -> OK bez záznamů. OK / INTERNAL. */
    int16_t (*log_read)(struct kivfs_dbb *b, uint64_t from, uint32_t max,
                        int (*emit)(void *ud, const struct kivfs_dbb_logrec *r),
                        void *ud);

    void (*close)(struct kivfs_dbb *b);
};

struct kivfs_dbb {
    const struct kivfs_dbb_ops *ops;
};

/* Mock: strom + archiv v paměti (archiv neomezený, ~100 B na operaci).
 * KIVFS_OK / KIVFS_ENOMEM. */
int kivfs_dbb_mock_open(struct kivfs_dbb **out);

#endif /* KIVFS_DB_BACKEND_H */
