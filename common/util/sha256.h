#ifndef KIVFS_SHA256_H
#define KIVFS_SHA256_H

/*
 * KIVFS – SHA-256 (FIPS 180-4)
 * ============================
 *
 * Otisk bloku = jeho jméno (bloky adresované obsahem, kap. 7.7.2: „pro každý
 * fragment se spočítá SHA256 otisk ... průběžně, jak data přicházejí“).
 * Používá FS (ověření a uložení bloku) i klient (dělení souboru), proto je
 * v common_lib a je přenositelná (Linux i macOS).
 *
 * Vlastní implementace bez závislosti na OpenSSL: ~150 řádků, ověřená
 * testovacími vektory FIPS 180-4 (test_sha256). Nezávislá na pořadí bajtů
 * stroje – slova se skládají z bajtů ručně.
 *
 * Kontext je obyčejná struktura (žádný malloc), takže se dá mít na zásobníku
 * nebo uvnitř sezení. Není thread-safe – jeden kontext = jedno vlákno.
 *
 *     struct kivfs_sha256 c;
 *     kivfs_sha256_init(&c);
 *     kivfs_sha256_update(&c, buf, n);     // libovolněkrát, libovolné kusy
 *     struct kivfs_digest d;
 *     kivfs_sha256_final(&c, &d);          // kontext je pak spotřebovaný
 *
 * Výsledek nezávisí na rozdělení dat do update (test to hlídá).
 */

#include <stddef.h>
#include <stdint.h>

#define KIVFS_DIGEST_LEN     32u
#define KIVFS_DIGEST_HEX_LEN 64u        /* bez '\0' */

/* Otisk jako struktura, ne holé pole: dá se přiřadit, předat hodnotou
 * a nehrozí záměna s jiným bufferem. Na drátě 32 surových bajtů
 * (pořadí bajtů neřeší – je to posloupnost bajtů, ne číslo). */
struct kivfs_digest {
    uint8_t b[KIVFS_DIGEST_LEN];
};

struct kivfs_sha256 {
    uint32_t h[8];        /* stav */
    uint64_t total;       /* počet zpracovaných bajtů (délka zprávy) */
    uint8_t  buf[64];     /* rozpracovaný blok */
    uint32_t buf_len;     /* 0..63 */
};

void kivfs_sha256_init  (struct kivfs_sha256 *c);
/* n = 0 je v pořádku (nic se nestane); data smí být NULL jen při n = 0. */
void kivfs_sha256_update(struct kivfs_sha256 *c, const void *data, size_t n);
/* Doplní padding a vydá otisk. Kontext je potom nepoužitelný, dokud se
 * znovu nezavolá init. */
void kivfs_sha256_final (struct kivfs_sha256 *c, struct kivfs_digest *out);

/* Celý buffer najednou (= init + update + final). */
void kivfs_sha256(const void *data, size_t n, struct kivfs_digest *out);

/* Porovnání otisků: 1 = shoda. Nemusí být v konstantním čase – otisk není
 * tajemství (na rozdíl od lístku, ten porovnává FS zvlášť). */
int  kivfs_digest_eq(const struct kivfs_digest *a, const struct kivfs_digest *b);

/* 64 malých hex znaků + '\0' do out (KIVFS_DIGEST_HEX_LEN + 1 B) – pro logy
 * a jména souborů úložiště. */
void kivfs_digest_hex(const struct kivfs_digest *d, char *out);

/* Opak: přesně 64 hex znaků (malá i velká písmena). KIVFS_OK nebo
 * KIVFS_EINVAL (jiná délka, nehex znak). Potřebuje úložiště při průchodu
 * adresářem (GC, později); zahrnuto kvůli symetrii a testu. */
int  kivfs_digest_from_hex(const char *hex, struct kivfs_digest *out);

#endif /* KIVFS_SHA256_H */
