#ifndef KIVFS_LOG_H
#define KIVFS_LOG_H

/*
 * KIVFS – jednoduchý log na stderr
 * ================================
 *
 * Jeden řádek: "HH:MM:SS.mmm [tag]   text". Řádek se nejdřív složí do
 * bufferu a vypíše jedním fputs, aby se řádky z různých vláken
 * neprokládaly uprostřed. Delší řádek se ořízne (512 B).
 *
 * Tag říká, kdo píše: "[main]", "[io]", "[wN]" (klientský worker),
 * "[nN]" (uzlový worker), "[peers]".
 */

#if defined(__GNUC__)
#  define KIVFS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#  define KIVFS_PRINTF(f, a)
#endif

void kivfs_log(const char *tag, const char *fmt, ...) KIVFS_PRINTF(2, 3);

/* conn_id (reactor.h) do čitelné podoby "generace:slot". */
#define KIVFS_CONN_FMT        "%u:%u"
#define KIVFS_CONN_ARGS(id)   (unsigned)((id) >> 32), (unsigned)((id) & 0xFFFFFFFFu)

#endif /* KIVFS_LOG_H */
