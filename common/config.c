#include "config.h"
#include "errors.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN   512          /* včetně '\0' */
#define FILE_MAX_SIZE  (64 * 1024)  /* konfigurace je malá; větší soubor = omyl */

enum section { SEC_NONE, SEC_NODE, SEC_NODES };

/* ------------------------------------------------------------------ */
/* Pomocné funkce                                                      */
/* ------------------------------------------------------------------ */

/* Zapíše chybu do err (pokud není NULL) a vrátí KIVFS_ECONFIG. */
static int fail(char *err, size_t err_size, unsigned line, const char *fmt, ...)
{
    if (err != NULL && err_size > 0) {
        int n = 0;
        if (line > 0)
            n = snprintf(err, err_size, "řádek %u: ", line);
        if (n >= 0 && (size_t)n < err_size) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(err + n, err_size - (size_t)n, fmt, ap);
            va_end(ap);
        }
    }
    return KIVFS_ECONFIG;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Ořízne bílé znaky na obou koncích; mění řetězec, vrací nový začátek. */
static char *trim(char *s)
{
    while (is_space(*s))
        s++;
    size_t len = strlen(s);
    while (len > 0 && is_space(s[len - 1]))
        s[--len] = '\0';
    return s;
}

/* Celý řetězec jako číslo v [min, max]. 0 = OK, -1 = neplatné. */
static int parse_uint(const char *s, unsigned long min, unsigned long max,
                      unsigned long *out)
{
    char *end;
    if (s[0] < '0' || s[0] > '9')         /* žádné "-1", "+5", " 5" */
        return -1;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static int cmp_node_id(const void *a, const void *b)
{
    uint32_t x = ((const struct kivfs_node_addr *)a)->node_id;
    uint32_t y = ((const struct kivfs_node_addr *)b)->node_id;
    return (x > y) - (x < y);
}

/* ------------------------------------------------------------------ */
/* Řádky                                                               */
/* ------------------------------------------------------------------ */

/* Stav parseru – co už bylo zadané (kvůli duplicitám a výchozím hodnotám). */
struct parse_state {
    enum section sec;
    int have_id, have_workers, have_max_conns;
    unsigned id_line;                          /* kde bylo id (pro hlášku) */
};

static int parse_node_key(struct kivfs_config *c, struct parse_state *st,
                          unsigned line, const char *key, const char *val,
                          char *err, size_t err_size)
{
    unsigned long v;

    if (strcmp(key, "id") == 0) {
        if (st->have_id)
            return fail(err, err_size, line, "klíč 'id' je zadaný dvakrát");
        if (parse_uint(val, 1, UINT32_MAX, &v) != 0)
            return fail(err, err_size, line, "neplatné id '%s' (1..4294967295)", val);
        c->self_id = (uint32_t)v;
        st->have_id = 1;
        st->id_line = line;
    } else if (strcmp(key, "workers") == 0) {
        if (st->have_workers)
            return fail(err, err_size, line, "klíč 'workers' je zadaný dvakrát");
        if (parse_uint(val, 1, KIVFS_CFG_MAX_WORKERS, &v) != 0)
            return fail(err, err_size, line, "neplatné workers '%s' (1..%u)",
                        val, KIVFS_CFG_MAX_WORKERS);
        c->workers = (unsigned)v;
        st->have_workers = 1;
    } else if (strcmp(key, "max_conns") == 0) {
        if (st->have_max_conns)
            return fail(err, err_size, line, "klíč 'max_conns' je zadaný dvakrát");
        if (parse_uint(val, 1, KIVFS_CFG_MAX_MAX_CONNS, &v) != 0)
            return fail(err, err_size, line, "neplatné max_conns '%s' (1..%u)",
                        val, KIVFS_CFG_MAX_MAX_CONNS);
        c->max_conns = (uint32_t)v;
        st->have_max_conns = 1;
    } else {
        return fail(err, err_size, line, "neznámý klíč '%s' v [node]", key);
    }
    return KIVFS_OK;
}

/* Řádek v [nodes]:  <id> = <host>:<port> */
static int parse_nodes_entry(struct kivfs_config *c, unsigned line,
                             const char *key, char *val,
                             char *err, size_t err_size)
{
    unsigned long id, port;

    if (parse_uint(key, 1, UINT32_MAX, &id) != 0)
        return fail(err, err_size, line, "neplatné node_id '%s' (1..4294967295)", key);

    for (size_t i = 0; i < c->n_nodes; i++)
        if (c->nodes[i].node_id == id)
            return fail(err, err_size, line, "duplicitní node_id %lu", id);

    if (c->n_nodes == KIVFS_CFG_MAX_NODES)
        return fail(err, err_size, line, "příliš mnoho uzlů (max %d)", KIVFS_CFG_MAX_NODES);

    char *colon = strrchr(val, ':');
    if (colon == NULL)
        return fail(err, err_size, line, "adresa '%s' musí mít tvar host:port", val);
    *colon = '\0';
    char *host = trim(val);
    char *port_s = trim(colon + 1);

    if (host[0] == '\0')
        return fail(err, err_size, line, "prázdný host u uzlu %lu", id);
    if (strchr(host, ':') != NULL)
        return fail(err, err_size, line, "host '%s': IPv6 adresy zatím nejsou podporované", host);
    for (const char *p = host; *p; p++)
        if (is_space(*p))
            return fail(err, err_size, line, "host '%s' obsahuje mezeru", host);
    if (strlen(host) >= KIVFS_CFG_HOST_MAX)
        return fail(err, err_size, line, "host je delší než %d znaků", KIVFS_CFG_HOST_MAX - 1);
    if (parse_uint(port_s, 1, 65535, &port) != 0)
        return fail(err, err_size, line, "neplatný port '%s' (1..65535)", port_s);

    struct kivfs_node_addr *n = &c->nodes[c->n_nodes++];
    n->node_id = (uint32_t)id;
    memcpy(n->host, host, strlen(host) + 1);
    n->port = (uint16_t)port;
    return KIVFS_OK;
}

static int parse_line(struct kivfs_config *c, struct parse_state *st,
                      unsigned line, char *text, char *err, size_t err_size)
{
    char *hash = strchr(text, '#');
    if (hash != NULL)
        *hash = '\0';
    char *s = trim(text);
    if (s[0] == '\0')
        return KIVFS_OK;                                   /* prázdný řádek */

    if (s[0] == '[') {
        size_t len = strlen(s);
        if (s[len - 1] != ']')
            return fail(err, err_size, line, "neuzavřená sekce '%s'", s);
        s[len - 1] = '\0';
        char *name = trim(s + 1);
        if (strcmp(name, "node") == 0)
            st->sec = SEC_NODE;
        else if (strcmp(name, "nodes") == 0)
            st->sec = SEC_NODES;
        else
            return fail(err, err_size, line, "neznámá sekce [%s]", name);
        return KIVFS_OK;
    }

    char *eq = strchr(s, '=');
    if (eq == NULL)
        return fail(err, err_size, line, "očekávám 'klíč = hodnota' nebo [sekce]");
    *eq = '\0';
    char *key = trim(s);
    char *val = trim(eq + 1);
    if (key[0] == '\0')
        return fail(err, err_size, line, "chybí klíč před '='");
    if (val[0] == '\0')
        return fail(err, err_size, line, "chybí hodnota klíče '%s'", key);

    switch (st->sec) {
    case SEC_NODE:  return parse_node_key(c, st, line, key, val, err, err_size);
    case SEC_NODES: return parse_nodes_entry(c, line, key, val, err, err_size);
    case SEC_NONE:  break;
    }
    return fail(err, err_size, line, "klíč '%s' mimo sekci ([node] nebo [nodes])", key);
}

/* Kontroly celku po přečtení všech řádků. */
static int validate(struct kivfs_config *c, const struct parse_state *st,
                    char *err, size_t err_size)
{
    if (!st->have_id)
        return fail(err, err_size, 0, "chybí [node] id");
    if (c->n_nodes == 0)
        return fail(err, err_size, 0, "sekce [nodes] je prázdná nebo chybí");
    if (kivfs_config_find(c, c->self_id) == NULL)
        return fail(err, err_size, st->id_line, "vlastní id %u není v [nodes]",
                    (unsigned)c->self_id);

    for (size_t i = 0; i < c->n_nodes; i++)
        for (size_t j = i + 1; j < c->n_nodes; j++)
            if (c->nodes[i].port == c->nodes[j].port
                && strcmp(c->nodes[i].host, c->nodes[j].host) == 0)
                return fail(err, err_size, 0, "uzly %u a %u mají stejnou adresu %s:%u",
                            (unsigned)c->nodes[i].node_id, (unsigned)c->nodes[j].node_id,
                            c->nodes[i].host, (unsigned)c->nodes[i].port);

    /* Každý další uzel drží jedno příchozí spojení – musí se vejít
     * do slotů reactoru, jinak by se nikdy nepřipojil. */
    if (c->max_conns < c->n_nodes)
        return fail(err, err_size, 0,
                    "max_conns (%u) je menší než počet uzlů (%zu) – ostatní uzly by se nepřipojily",
                    (unsigned)c->max_conns, c->n_nodes);
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Veřejné funkce                                                      */
/* ------------------------------------------------------------------ */

int kivfs_config_parse(const char *text, struct kivfs_config *out,
                       char *err, size_t err_size)
{
    /* Parsujeme do lokální kopie, aby *out při chybě zůstal beze změny.
     * static by nebyl thread-safe; ~17 KB na zásobníku je v pořádku. */
    struct kivfs_config c;
    struct parse_state st;
    char buf[LINE_MAX_LEN];
    unsigned line = 0;
    int rc;

    if (text == NULL || out == NULL)
        return KIVFS_EINVAL;

    memset(&c, 0, sizeof c);
    memset(&st, 0, sizeof st);
    c.workers = KIVFS_CFG_DEFAULT_WORKERS;
    c.max_conns = KIVFS_CFG_DEFAULT_MAX_CONNS;

    const char *p = text;
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        line++;
        if (len >= sizeof buf)
            return fail(err, err_size, line, "řádek je delší než %d znaků", LINE_MAX_LEN - 1);
        memcpy(buf, p, len);
        buf[len] = '\0';
        if ((rc = parse_line(&c, &st, line, buf, err, err_size)) != KIVFS_OK)
            return rc;
        p = nl ? nl + 1 : p + len;
    }

    if ((rc = validate(&c, &st, err, err_size)) != KIVFS_OK)
        return rc;

    qsort(c.nodes, c.n_nodes, sizeof c.nodes[0], cmp_node_id);
    *out = c;
    return KIVFS_OK;
}

int kivfs_config_load(const char *path, struct kivfs_config *out,
                      char *err, size_t err_size)
{
    if (path == NULL || out == NULL)
        return KIVFS_EINVAL;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (err != NULL && err_size > 0)
            snprintf(err, err_size, "%s: %s", path, strerror(errno));
        return KIVFS_EIO;
    }

    char *text = malloc(FILE_MAX_SIZE + 1);
    if (text == NULL) {
        fclose(f);
        return KIVFS_ENOMEM;
    }
    size_t n = fread(text, 1, FILE_MAX_SIZE + 1, f);
    int read_err = ferror(f);
    int e = errno;
    fclose(f);

    int rc;
    if (read_err) {
        if (err != NULL && err_size > 0)
            snprintf(err, err_size, "%s: chyba čtení: %s", path, strerror(e));
        rc = KIVFS_EIO;
    } else if (n > FILE_MAX_SIZE) {
        if (err != NULL && err_size > 0)
            snprintf(err, err_size, "%s: soubor je větší než %d B", path, FILE_MAX_SIZE);
        rc = KIVFS_ECONFIG;
    } else if (memchr(text, '\0', n) != NULL) {
        if (err != NULL && err_size > 0)
            snprintf(err, err_size, "%s: soubor obsahuje nulový bajt (není textový?)", path);
        rc = KIVFS_ECONFIG;
    } else {
        text[n] = '\0';
        char detail[256] = "";
        rc = kivfs_config_parse(text, out, detail, sizeof detail);
        if (rc != KIVFS_OK && err != NULL && err_size > 0)
            snprintf(err, err_size, "%s: %s", path, detail);
    }
    free(text);
    return rc;
}

const struct kivfs_node_addr *kivfs_config_find(const struct kivfs_config *c,
                                                uint32_t node_id)
{
    for (size_t i = 0; i < c->n_nodes; i++)
        if (c->nodes[i].node_id == node_id)
            return &c->nodes[i];
    return NULL;
}

/* FNV-1a 64: jednoduchý a pro naše účely (odhalit omylem jiný seznam)
 * dostatečný; není to kryptografický hash. */
#define FNV64_OFFSET 0xcbf29ce484222325ULL
#define FNV64_PRIME  0x100000001b3ULL

static uint64_t fnv_bytes(uint64_t h, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

uint64_t kivfs_config_fingerprint(const struct kivfs_config *c)
{
    uint64_t h = FNV64_OFFSET;
    /* nodes[] je seřazené podle node_id, takže pořadí řádků v souboru
     * otisk neovlivní. Čísla se hashují v pevném pořadí bajtů (big-endian)
     * a před hostem jde jeho délka, aby se "ab"+"c" nerovnalo "a"+"bc". */
    for (size_t i = 0; i < c->n_nodes; i++) {
        const struct kivfs_node_addr *n = &c->nodes[i];
        uint8_t num[4];
        size_t hl = strlen(n->host);

        num[0] = (uint8_t)(n->node_id >> 24); num[1] = (uint8_t)(n->node_id >> 16);
        num[2] = (uint8_t)(n->node_id >> 8);  num[3] = (uint8_t)n->node_id;
        h = fnv_bytes(h, num, 4);
        num[0] = (uint8_t)(hl >> 8); num[1] = (uint8_t)hl;
        h = fnv_bytes(h, num, 2);
        h = fnv_bytes(h, n->host, hl);
        num[0] = (uint8_t)(n->port >> 8); num[1] = (uint8_t)n->port;
        h = fnv_bytes(h, num, 2);
    }
    return h;
}
