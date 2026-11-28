#ifndef KIVFS_FS_STORE_H
#define KIVFS_FS_STORE_H

/*
 * KIVFS – úložiště bloků modulu FS
 * ================================
 *
 * Fyzické uložení obsahu souborů (kap. 7.7). Soubor se dělí na bloky pevné
 * velikosti (fragmenty, 7.7.2); blok je NEMĚNNÝ a jmenuje se podle SHA-256
 * svého obsahu. Z toho plyne:
 *   - stejný obsah = stejné jméno = na uzlu jen jedna kopie (deduplikace
 *     7.7.3 bez dodatečné úlohy a bez počítadel odkazů),
 *   - put je idempotentní: opakované nebo souběžné uložení stejného bloku
 *     dopadne stejně (důležité pro opakování po výpadku a pro replikaci),
 *   - úložiště NEVÍ nic o souborech, verzích ani zámcích – to jsou metadata
 *     v DB. Kdo na blok odkazuje, řeší DB; úklid neodkazovaných bloků je
 *     GC (později).
 *
 * Backend je vyměnitelný jako DB backend (db/backend.h):
 *   dir   adresář na disku (výchozí, jediný pro provoz),
 *   mock  v paměti (unit testy modulu FS bez disku).
 *
 * Volání jsou SYNCHRONNÍ a volá je jen worker FS (backend nemusí být
 * thread-safe). Worker smí blokovat na disku, ne na síti. Blok do 15 MiB
 * znamená jednotky až desítky ms – vědomé omezení, výhledově samostatné
 * I/O vlákno (pravidla pro více workerů to dovolují).
 *
 * Výsledek je přímo stav na drátě (enum kivfs_status, jako DB backend);
 * detail chyby (errno, cesta) backend zaloguje přes log callback.
 *
 * Bezpečnost: put VŽDY sám přepočítá otisk a porovná ho s h. Volající
 * (klient, jiný uzel) může poslat cokoli – bez kontroly by šlo pod cizí
 * otisk podvrhnout jiná data a přes deduplikaci tak změnit obsah cizích
 * souborů. Kontrola je tady, aby ji nešlo obejít žádnou cestou.
 */

#include <stdint.h>
#include "proto/fsproto.h"     /* KIVFS_FS_BLOCK_MAX */
#include "util/sha256.h"

struct kivfs_store;

struct kivfs_store_ops {
    /* Uloží blok h = data[0..len). Podmínky: 1 <= len <= KIVFS_FS_BLOCK_MAX,
     * SHA-256(data) == h.
     *   OK         uloženo, nebo už existoval se stejnou délkou (nic se
     *              nepřepisuje – idempotence)
     *   MALFORMED  otisk nesedí, len mimo rozsah
     *   NO_SPACE   došlo místo (ENOSPC, EDQUOT, EFBIG) – nic nezůstane
     *   INTERNAL   jiná chyba úložiště – nic nezůstane
     * Atomicita (dir): data -> tmp/ -> fsync -> rename na jméno otisku ->
     * fsync adresáře. Pod jménem otisku tedy nikdy neleží půlka bloku;
     * po pádu zůstane nanejvýš soubor v tmp/ (smaže ho další open).
     * Existující soubor s JINOU délkou se bere jako poškozený a nahradí se. */
    int16_t (*put)(struct kivfs_store *s, const struct kivfs_digest *h,
                   const void *data, uint32_t len);

    /* Načte blok h do buf (cap B), délka do *len. buf typicky ukazuje
     * přímo do payloadu odpovědi (bez kopie navíc).
     *   OK
     *   NOT_FOUND  blok tu není – nebo byl poškozený (viz verify)
     *   INTERNAL   cap < délka bloku, chyba čtení
     * verify != 0: přepočítá otisk; nesedí-li, blok se přesune do bad/
     * (karanténa, ne smazání – kvůli rozboru), zaloguje se a vrátí se
     * NOT_FOUND. „Poškozený = chybí“: od FS-2 si ho FS stejnou cestou
     * dočte od jiného uzlu. */
    int16_t (*get)(struct kivfs_store *s, const struct kivfs_digest *h,
                   void *buf, uint32_t cap, uint32_t *len, int verify);

    /* Je blok h přítomen? Délka do *len (smí být NULL). Obsah neověřuje.
     * OK / NOT_FOUND / INTERNAL. (SEAL při commitu: všechny bloky
     * přítomné a délky sedí s rozložením souboru.) */
    int16_t (*has)(struct kivfs_store *s, const struct kivfs_digest *h, uint32_t *len);

    /* Odstraní blok h. OK / NOT_FOUND / INTERNAL. V provozu jen GC
     * (později) – úložiště neví, jestli na blok něco odkazuje. */
    int16_t (*remove)(struct kivfs_store *s, const struct kivfs_digest *h);

    /* (později, GC) scan: průchod všemi bloky (h, len, mtime_ms). */

    void (*close)(struct kivfs_store *s);
};

struct kivfs_store {
    const struct kivfs_store_ops *ops;
};

/* Kam backend loguje (worker FS předá kivfs_mod_log). Smí být NULL. */
typedef void (*kivfs_store_log_fn)(void *ud, const char *msg);

struct kivfs_store_dir_cfg {
    const char        *root;       /* [fs] root – POVINNÝ, absolutní cesta */
    int                fsync;      /* [fs] fsync = yes|no (výchozí yes);
                                    * no jen pro testy – po pádu stroje se
                                    * může ztratit blok, který už byl
                                    * potvrzený (metadata by pak ukazovala
                                    * na chybějící data) */
    kivfs_store_log_fn log;
    void              *log_ud;
};

/*
 * Rozložení adresáře (vytvoří se, pokud chybí):
 *   <root>/LOCK                 flock – root smí používat jen jeden proces
 *   <root>/blocks/ab/cd/<hex>   blok; ab, cd = první 2 a další 2 znaky otisku
 *                               (max. 65 536 adresářů, v každém málo souborů)
 *   <root>/tmp/                 rozpracované zápisy; open ho vyprázdní
 *   <root>/bad/                 karanténa poškozených bloků
 * Práva: adresáře 0700, bloky 0600 (data uživatelů čte jen proces FS).
 *
 * KIVFS_OK; KIVFS_ECONFIG (root chybí / není absolutní / nejde vytvořit /
 * není zapisovatelný / root už drží jiný proces – důvod v err);
 * KIVFS_ENOMEM. err smí být NULL.
 */
int kivfs_store_dir_open(const struct kivfs_store_dir_cfg *cfg,
                         struct kivfs_store **out, char *err, size_t errlen);

/* Mock: bloky v paměti (žádný limit kromě paměti), stejná sémantika jako
 * dir včetně kontroly otisku. KIVFS_OK / KIVFS_ENOMEM. */
int kivfs_store_mock_open(struct kivfs_store **out);

#endif /* KIVFS_FS_STORE_H */
