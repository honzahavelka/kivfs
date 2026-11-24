#ifndef KIVFS_CONFIG_H
#define KIVFS_CONFIG_H

/*
 * KIVFS – minimální INI parser
 * ============================
 *
 * Obecný: neví nic o modulech KIVFS, jen načte soubor do dvojic
 * (sekce, klíč) -> hodnota. Co je povinné, povolené a jaký má význam,
 * řeší vrstva nad ním (server/modconf.h).
 *
 * Syntaxe:
 *     ; komentář            # také komentář
 *     [sekce]
 *     klic = hodnota        ; komentář za hodnotou (před ; nebo # mezera)
 *
 *   - mezery kolem názvů, '=' a hodnoty se ignorují; hodnota bez uvozovek
 *   - názvy sekcí a klíčů: písmena, číslice, '_' a '-'; rozlišují se
 *     velká a malá písmena
 *   - prázdná hodnota je povolená ("klic =")
 *
 * Chyby (vše KIVFS_ECONFIG, popis "soubor:řádek: důvod" v err):
 *   řádek bez '=', klíč před první sekcí, neplatný název, duplicitní
 *   sekce nebo klíč, řádek delší než KIVFS_CONFIG_LINE_MAX.
 * Duplicita je chyba schválně: u "port = 1" ... "port = 2" by jinak
 * nebylo jasné, která hodnota platí.
 *
 * Soubor se čte jednou při startu; změna za běhu se projeví až po
 * restartu procesu.
 */

#include <stddef.h>

#define KIVFS_CONFIG_LINE_MAX 1024

struct kivfs_config;

/* Načte soubor. Chyby: KIVFS_EIO (nejde otevřít/přečíst, errno),
 * KIVFS_ECONFIG (syntaxe), KIVFS_ENOMEM. err (smí být NULL) dostane
 * čitelný popis. */
int  kivfs_config_load(const char *path, struct kivfs_config **out,
                       char *err, size_t errlen);

void kivfs_config_free(struct kivfs_config *c);

/* Cesta, ze které byl soubor načten (pro chybové zprávy vyšší vrstvy). */
const char *kivfs_config_path(const struct kivfs_config *c);

/* Hodnota klíče, nebo NULL, pokud sekce/klíč chybí. Ukazatel platí do
 * kivfs_config_free. */
const char *kivfs_config_get(const struct kivfs_config *c,
                             const char *section, const char *key);

/* Číslo řádku klíče (nebo sekce, je-li key NULL); 0 = neexistuje. */
int  kivfs_config_line(const struct kivfs_config *c,
                       const char *section, const char *key);

/* Projde sekce v pořadí souboru. Nenulový návrat fn průchod ukončí
 * a funkce ho vrátí; jinak 0. */
int  kivfs_config_foreach_section(const struct kivfs_config *c,
                                  int (*fn)(const char *section, int line, void *ud),
                                  void *ud);

/* Projde klíče jedné sekce v pořadí souboru (neexistující sekce = nic).
 * Návrat jako u foreach_section. */
int  kivfs_config_foreach(const struct kivfs_config *c, const char *section,
                          int (*fn)(const char *key, const char *value, int line, void *ud),
                          void *ud);

#endif /* KIVFS_CONFIG_H */
