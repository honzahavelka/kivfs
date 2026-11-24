/*
 * KIVFS – minimální INI parser (implementace), viz config.h
 *
 * Uložení: pole sekcí a pole položek (sekce, klíč, hodnota, řádek)
 * v pořadí souboru. Vyhledávání je lineární – konfigurace má desítky
 * řádků a čte se jednou při startu.
 */
#define _POSIX_C_SOURCE 200809L

#include "util/config.h"
#include "proto/errors.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    size_t sec;          /* index do sections */
    char  *key;
    char  *value;
    int    line;
};

struct section {
    char *name;
    int   line;
};

struct kivfs_config {
    char           *path;
    struct section *secs;
    size_t          nsec, capsec;
    struct entry   *ents;
    size_t          nent, capent;
};

/* ------------------------------------------------------------------ */
/* Pomocné                                                             */
/* ------------------------------------------------------------------ */

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL)
        memcpy(d, s, n);
    return d;
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* Ořízne mezery z obou stran (na místě), vrátí začátek. */
static char *trim(char *s)
{
    while (is_space(*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && is_space(s[n - 1]))
        s[--n] = '\0';
    return s;
}

static int valid_name(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s; s++) {
        char ch = *s;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
              || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return 0;
    }
    return 1;
}

/* Odřízne komentář za hodnotou: ';' nebo '#' na začátku nebo po mezeře. */
static void strip_inline_comment(char *s)
{
    for (char *p = s; *p; p++) {
        if ((*p == ';' || *p == '#') && (p == s || is_space(p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

static long find_section(const struct kivfs_config *c, const char *name)
{
    for (size_t i = 0; i < c->nsec; i++)
        if (strcmp(c->secs[i].name, name) == 0)
            return (long)i;
    return -1;
}

static const struct entry *find_entry(const struct kivfs_config *c,
                                      const char *section, const char *key)
{
    long s = find_section(c, section);
    if (s < 0)
        return NULL;
    for (size_t i = 0; i < c->nent; i++)
        if (c->ents[i].sec == (size_t)s && strcmp(c->ents[i].key, key) == 0)
            return &c->ents[i];
    return NULL;
}

static int add_section(struct kivfs_config *c, const char *name, int line)
{
    if (c->nsec == c->capsec) {
        size_t cap = c->capsec ? c->capsec * 2 : 8;
        struct section *n = realloc(c->secs, cap * sizeof *n);
        if (n == NULL)
            return KIVFS_ENOMEM;
        c->secs = n;
        c->capsec = cap;
    }
    char *d = dup_str(name);
    if (d == NULL)
        return KIVFS_ENOMEM;
    c->secs[c->nsec].name = d;
    c->secs[c->nsec].line = line;
    c->nsec++;
    return KIVFS_OK;
}

static int add_entry(struct kivfs_config *c, const char *key, const char *value, int line)
{
    if (c->nent == c->capent) {
        size_t cap = c->capent ? c->capent * 2 : 16;
        struct entry *n = realloc(c->ents, cap * sizeof *n);
        if (n == NULL)
            return KIVFS_ENOMEM;
        c->ents = n;
        c->capent = cap;
    }
    char *k = dup_str(key);
    char *v = dup_str(value);
    if (k == NULL || v == NULL) {
        free(k);
        free(v);
        return KIVFS_ENOMEM;
    }
    c->ents[c->nent].sec   = c->nsec - 1;   /* vždy poslední otevřená sekce */
    c->ents[c->nent].key   = k;
    c->ents[c->nent].value = v;
    c->ents[c->nent].line  = line;
    c->nent++;
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Načtení                                                             */
/* ------------------------------------------------------------------ */

static int parse_line(struct kivfs_config *c, char *raw, int line, char *err, size_t errlen)
{
    char *s = trim(raw);
    if (*s == '\0' || *s == ';' || *s == '#')
        return KIVFS_OK;

    if (*s == '[') {
        char *end = strchr(s, ']');
        if (end == NULL) {
            set_err(err, errlen, "%s:%d: chybí ']'", c->path, line);
            return KIVFS_ECONFIG;
        }
        *end = '\0';
        char *rest = end + 1;
        strip_inline_comment(rest);
        char *name = trim(s + 1);
        if (*trim(rest) != '\0' || !valid_name(name)) {
            set_err(err, errlen, "%s:%d: neplatný název sekce", c->path, line);
            return KIVFS_ECONFIG;
        }
        long dup = find_section(c, name);
        if (dup >= 0) {
            set_err(err, errlen, "%s:%d: sekce [%s] už je na řádku %d",
                    c->path, line, name, c->secs[dup].line);
            return KIVFS_ECONFIG;
        }
        return add_section(c, name, line);
    }

    char *eq = strchr(s, '=');
    if (eq == NULL) {
        set_err(err, errlen, "%s:%d: očekáváno 'klíč = hodnota'", c->path, line);
        return KIVFS_ECONFIG;
    }
    *eq = '\0';
    char *key = trim(s);
    char *value = eq + 1;
    strip_inline_comment(value);
    value = trim(value);

    if (c->nsec == 0) {
        set_err(err, errlen, "%s:%d: klíč '%s' před první [sekcí]", c->path, line, key);
        return KIVFS_ECONFIG;
    }
    if (!valid_name(key)) {
        set_err(err, errlen, "%s:%d: neplatný název klíče", c->path, line);
        return KIVFS_ECONFIG;
    }
    const struct entry *dup = find_entry(c, c->secs[c->nsec - 1].name, key);
    if (dup != NULL) {
        set_err(err, errlen, "%s:%d: klíč '%s' už je na řádku %d",
                c->path, line, key, dup->line);
        return KIVFS_ECONFIG;
    }
    return add_entry(c, key, value, line);
}

int kivfs_config_load(const char *path, struct kivfs_config **out,
                      char *err, size_t errlen)
{
    if (out == NULL)
        return KIVFS_EINVAL;
    *out = NULL;
    if (path == NULL)
        return KIVFS_EINVAL;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        set_err(err, errlen, "%s: %s", path, strerror(errno));
        return KIVFS_EIO;
    }

    struct kivfs_config *c = calloc(1, sizeof *c);
    if (c == NULL || (c->path = dup_str(path)) == NULL) {
        free(c);
        fclose(f);
        return KIVFS_ENOMEM;
    }

    /* +2: '\n' a '\0' – řádek s přesně LINE_MAX znaky se ještě vejde */
    char buf[KIVFS_CONFIG_LINE_MAX + 2];
    int line = 0;
    int rc = KIVFS_OK;
    while (rc == KIVFS_OK && fgets(buf, sizeof buf, f) != NULL) {
        line++;
        size_t n = strlen(buf);
        if (n == sizeof buf - 1 && buf[n - 1] != '\n' && !feof(f)) {
            set_err(err, errlen, "%s:%d: řádek delší než %d znaků",
                    path, line, KIVFS_CONFIG_LINE_MAX);
            rc = KIVFS_ECONFIG;
            break;
        }
        rc = parse_line(c, buf, line, err, errlen);
    }
    if (rc == KIVFS_OK && ferror(f)) {
        set_err(err, errlen, "%s: chyba čtení", path);
        rc = KIVFS_EIO;
    }
    fclose(f);

    if (rc != KIVFS_OK) {
        if (rc == KIVFS_ENOMEM)
            set_err(err, errlen, "%s: došla paměť", path);
        kivfs_config_free(c);
        return rc;
    }
    *out = c;
    return KIVFS_OK;
}

void kivfs_config_free(struct kivfs_config *c)
{
    if (c == NULL)
        return;
    for (size_t i = 0; i < c->nsec; i++)
        free(c->secs[i].name);
    for (size_t i = 0; i < c->nent; i++) {
        free(c->ents[i].key);
        free(c->ents[i].value);
    }
    free(c->secs);
    free(c->ents);
    free(c->path);
    free(c);
}

/* ------------------------------------------------------------------ */
/* Dotazy                                                              */
/* ------------------------------------------------------------------ */

const char *kivfs_config_path(const struct kivfs_config *c)
{
    return c != NULL ? c->path : "";
}

const char *kivfs_config_get(const struct kivfs_config *c,
                             const char *section, const char *key)
{
    if (c == NULL || section == NULL || key == NULL)
        return NULL;
    const struct entry *e = find_entry(c, section, key);
    return e != NULL ? e->value : NULL;
}

int kivfs_config_line(const struct kivfs_config *c, const char *section, const char *key)
{
    if (c == NULL || section == NULL)
        return 0;
    if (key == NULL) {
        long s = find_section(c, section);
        return s >= 0 ? c->secs[s].line : 0;
    }
    const struct entry *e = find_entry(c, section, key);
    return e != NULL ? e->line : 0;
}

int kivfs_config_foreach_section(const struct kivfs_config *c,
                                 int (*fn)(const char *section, int line, void *ud),
                                 void *ud)
{
    if (c == NULL || fn == NULL)
        return 0;
    for (size_t i = 0; i < c->nsec; i++) {
        int r = fn(c->secs[i].name, c->secs[i].line, ud);
        if (r != 0)
            return r;
    }
    return 0;
}

int kivfs_config_foreach(const struct kivfs_config *c, const char *section,
                         int (*fn)(const char *key, const char *value, int line, void *ud),
                         void *ud)
{
    if (c == NULL || section == NULL || fn == NULL)
        return 0;
    long s = find_section(c, section);
    if (s < 0)
        return 0;
    for (size_t i = 0; i < c->nent; i++) {
        if (c->ents[i].sec != (size_t)s)
            continue;
        int r = fn(c->ents[i].key, c->ents[i].value, c->ents[i].line, ud);
        if (r != 0)
            return r;
    }
    return 0;
}
