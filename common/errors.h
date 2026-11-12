#ifndef KIVFS_ERRORS_H
#define KIVFS_ERRORS_H

/*
 * KIVFS – LOKÁLNÍ návratové kódy funkcí
 * =====================================
 *
 * Tyto kódy vracejí funkce uvnitř jednoho procesu a NIKDY se neposílají
 * po síti. Konvence (stejná jako u většiny systémových API v C):
 *     0               = úspěch (KIVFS_OK)
 *     záporné číslo   = chyba (některá z hodnot níže)
 *     kladné číslo    = užitečná hodnota (např. fd nebo počet bajtů),
 *                       pokud to funkce výslovně uvádí
 *
 * Proč dva druhy chybových kódů v projektu?
 *   - enum kivfs_err (tento soubor): "co se pokazilo u mě" – např. spadlo
 *     spojení, přišla poškozená data, došla paměť. Řeší je volající lokálně.
 *   - enum kivfs_status (protocol.h): "co odpovídám protistraně" – posílá se
 *     v poli return_value hlavičky, např. "soubor neexistuje".
 *   Kdyby to byl jeden enum, snadno by se po síti omylem poslal kód typu
 *   "došla mi paměť", který protistraně nic neříká.
 *
 * Rozsahy podle modulů (aby šlo z čísla v logu hned poznat, odkud chyba je):
 *    -1 ..  -19   protocol / packet
 *   -20 ..  -39   socket / channel
 *   -40 ..  -59   fronta / server
 *
 * Typické použití:
 *     int rc = kivfs_chan_call(&ch, &req, &resp);
 *     if (rc != KIVFS_OK) {
 *         fprintf(stderr, "call na %s selhal: %s\n", ch.peer_addr, kivfs_strerror(rc));
 *         kivfs_chan_close(&ch);
 *     }
 */

enum kivfs_err {
    KIVFS_OK          =  0,

    /* ---- protocol / packet ---- */
    KIVFS_EBADMAGIC   = -1,   /* Hlavička nezačíná KIVFS_MAGIC. Buď mluví cizí
                               * program, nebo jsme ztratili hranici paketu
                               * v TCP proudu. Z obojího se nedá zotavit
                               * -> spojení zavřít. */
    KIVFS_ETOOBIG     = -2,   /* data_size > KIVFS_MAX_PAYLOAD. Payload se
                               * vůbec nealokuje (ochrana paměti). */
    KIVFS_EMALFORMED  = -3,   /* Strukturálně vadný payload: délka záznamu
                               * přesahuje konec dat, odpověď nemá povinný
                               * první záznam apod. */
    KIVFS_ENOMEM      = -4,   /* Selhal malloc/realloc. */
    KIVFS_EEND        = -5,   /* Reader: požadovaný záznam už v payloadu není
                               * (paket má méně záznamů, než handler čeká). */
    KIVFS_ETYPE       = -6,   /* Reader: záznam existuje, ale nesedí typ –
                               * např. čteme u32 a záznam nemá 4 B, nebo se
                               * řetězec nevejde do cílového bufferu. */

    /* ---- socket / channel ---- */
    KIVFS_ECLOSED     = -20,  /* Protistrana korektně zavřela spojení PŘED
                               * prvním bajtem dalšího paketu. Běžný konec
                               * spojení, nikoli chyba v pravém smyslu. */
    KIVFS_ETRUNC      = -21,  /* Spojení skončilo UPROSTŘED paketu (pád
                               * protistrany, výpadek sítě). Data jsou neúplná. */
    KIVFS_ETIMEOUT    = -22,  /* Vypršel timeout čtení/zápisu/připojení. */
    KIVFS_EAGAIN      = -23,  /* Neblokující socket: operace by teď blokovala,
                               * zkus to znovu, až epoll ohlásí připravenost.
                               * Vrací jen neblokující funkce (pro reactor). */
    KIVFS_EIO         = -24,  /* Jiná chyba socketu; detail je v errno. */
    KIVFS_ERESOLVE    = -25,  /* Nepodařilo se přeložit jméno/adresu
                               * (getaddrinfo). */
    KIVFS_EUNEXPECTED = -26,  /* Přišla odpověď, která nepatří k odeslanému
                               * požadavku (jiný kód operace nebo jiný ref).
                               * Při "jeden požadavek na kanál" by se to nemělo
                               * stát – značí chybu v protistraně. */

    /* ---- fronta / server ---- */
    KIVFS_ESHUTDOWN   = -40,  /* Komponenta (fronta, reactor) se ukončuje.
                               * Pro workera signál "skonči". */
    KIVFS_EINVAL      = -41   /* Neplatný argument funkce (např. nulová
                               * kapacita fronty) – chyba volajícího. */
};

/* Vrátí statický textový popis chyby pro logy (řetězec se neuvolňuje).
 * Pro neznámý kód vrací "unknown error". */
const char *kivfs_strerror(int err);

#endif /* KIVFS_ERRORS_H */
