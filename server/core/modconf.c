/*
 * KIVFS – konfigurace z pohledu modulu (implementace), viz modconf.h
 */
#define _POSIX_C_SOURCE 200809L

#include "core/modconf.h"
#include "proto/errors.h"
#include "proto/hello.h"
#include "net/socket.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MAX_CONNS      256
#define DEFAULT_RX_TIMEOUT_MS  10000
#define MAX_NODE_ID            999999999u   /* "node" + max. 9 číslic */

/* Kdo koho volá: kivfs_module_may_call (hello.h) – pevně v kódu, ne
 * v konfiguraci, je to architektura systému. Závislost = modul, který
 * volám na STEJNÉM uzlu (Synce ostatních uzlů jsou v [cluster]). */
static int is_dep(enum kivfs_module self, int m)
{
    return m != (int)self && kivfs_module_may_call(self, (uint32_t)m);
}

static const char *const g_base_keys[] = {
    "ip", "port", "bind", "max_conns", "max_out_conns", "max_pending",
    "connect_timeout_ms", "rx_timeout_ms", NULL
};

/* ------------------------------------------------------------------ */
/* Kontext validace a hlášení chyb                                     */
/* ------------------------------------------------------------------ */

struct ctx {
    const struct kivfs_config *c;
    struct kivfs_modconf      *mc;
    const char *const         *extra;
    char                      *err;
    size_t                     errlen;
    int                        sync_port;    /* 0 = [sync] port chybí */
};

static int fail(struct ctx *x, int line, const char *fmt, ...)
{
    if (x->err != NULL && x->errlen > 0) {
        int n = line > 0
            ? snprintf(x->err, x->errlen, "%s:%d: ", kivfs_config_path(x->c), line)
            : snprintf(x->err, x->errlen, "%s: ", kivfs_config_path(x->c));
        if (n >= 0 && (size_t)n < x->errlen) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(x->err + n, x->errlen - (size_t)n, fmt, ap);
            va_end(ap);
        }
    }
    return KIVFS_ECONFIG;
}

static int in_list(const char *s, const char *const *list)
{
    for (; list != NULL && *list != NULL; list++)
        if (strcmp(s, *list) == 0)
            return 1;
    return 0;
}

/* Striktní desítkové číslo v [min, max]; 0 = OK. */
static int parse_uint(const char *s, unsigned long min, unsigned long max, unsigned long *out)
{
    if (*s < '0' || *s > '9')
        return -1;
    unsigned long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        if (v > (max - (unsigned long)(*s - '0')) / 10)
            return -1;                          /* přetečení nad max */
        v = v * 10 + (unsigned long)(*s - '0');
    }
    if (v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static int host_ok(const char *h)
{
    if (*h == '\0' || strlen(h) >= KIVFS_HOST_MAX)
        return 0;
    for (; *h; h++)
        if (*h == ' ' || *h == '\t' || *h == '[' || *h == ']')
            return 0;
    return 1;
}

/* "host", "host:port", "[ipv6]", "[ipv6]:port". default_port 0 = port
 * povinný. 0 = OK, -1 = syntaxe, -2 = chybí port. */
static int parse_endpoint(const char *s, int default_port, struct kivfs_endpoint *ep)
{
    char host[KIVFS_HOST_MAX];
    const char *port_str = NULL;
    size_t hl;

    if (*s == '[') {
        const char *end = strchr(s, ']');
        if (end == NULL)
            return -1;
        hl = (size_t)(end - s - 1);
        if (hl >= sizeof host)
            return -1;
        memcpy(host, s + 1, hl);
        if (end[1] == ':')
            port_str = end + 2;
        else if (end[1] != '\0')
            return -1;
    } else {
        const char *colon = strchr(s, ':');
        if (colon != NULL && strchr(colon + 1, ':') != NULL)
            return -1;                          /* IPv6 bez [] – port nejde poznat */
        hl = colon ? (size_t)(colon - s) : strlen(s);
        if (hl >= sizeof host)
            return -1;
        memcpy(host, s, hl);
        if (colon != NULL)
            port_str = colon + 1;
    }
    host[hl] = '\0';
    if (!host_ok(host))
        return -1;

    unsigned long port;
    if (port_str != NULL) {
        if (parse_uint(port_str, 1, 65535, &port) != 0)
            return -1;
    } else if (default_port > 0) {
        port = (unsigned long)default_port;
    } else {
        return -2;
    }
    memcpy(ep->host, host, hl + 1);
    ep->port = (uint16_t)port;
    return 0;
}

/* ip (bez portu, IPv6 smí být v [] i bez) + port ze sekce. */
static int parse_module_addr(struct ctx *x, const char *sec, struct kivfs_endpoint *ep)
{
    const char *ip = kivfs_config_get(x->c, sec, "ip");
    const char *pt = kivfs_config_get(x->c, sec, "port");
    int sec_line = kivfs_config_line(x->c, sec, NULL);
    if (ip == NULL)
        return fail(x, sec_line, "[%s]: chybí povinný klíč 'ip'", sec);
    if (pt == NULL)
        return fail(x, sec_line, "[%s]: chybí povinný klíč 'port'", sec);

    char host[KIVFS_HOST_MAX];
    size_t n = strlen(ip);
    if (n >= 2 && ip[0] == '[' && ip[n - 1] == ']') {
        ip++;
        n -= 2;
    }
    if (n >= sizeof host)
        return fail(x, kivfs_config_line(x->c, sec, "ip"), "[%s] ip: příliš dlouhé", sec);
    memcpy(host, ip, n);
    host[n] = '\0';
    if (!host_ok(host))
        return fail(x, kivfs_config_line(x->c, sec, "ip"), "[%s] ip: neplatná adresa '%s'", sec, ip);

    unsigned long port;
    if (parse_uint(pt, 1, 65535, &port) != 0)
        return fail(x, kivfs_config_line(x->c, sec, "port"),
                    "[%s] port: '%s' není číslo 1..65535", sec, pt);
    memcpy(ep->host, host, n + 1);
    ep->port = (uint16_t)port;
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Sekce                                                               */
/* ------------------------------------------------------------------ */

static int check_section_name(const char *sec, int line, void *ud)
{
    struct ctx *x = ud;
    if (strcmp(sec, "cluster") == 0)
        return 0;
    for (int m = KIVFS_MOD_AUTH; m < KIVFS_MOD_COUNT; m++)
        if (strcmp(sec, kivfs_module_name((enum kivfs_module)m)) == 0)
            return 0;
    return fail(x, line, "neznámá sekce [%s] (povolené: cluster, auth, sync, vfs, db, fs)", sec);
}

/* nodeN -> N; 0 = není platné jméno uzlu */
static uint32_t node_id_of(const char *key)
{
    unsigned long id;
    if (strncmp(key, "node", 4) != 0 || key[4] == '0'
        || parse_uint(key + 4, 1, MAX_NODE_ID, &id) != 0)
        return 0;
    return (uint32_t)id;
}

static int cluster_key(const char *key, const char *value, int line, void *ud)
{
    struct ctx *x = ud;
    struct kivfs_modconf *mc = x->mc;

    if (strcmp(key, "local") == 0 || strcmp(key, "op_timeout_ms") == 0)
        return 0;                               /* řeší se po načtení uzlů */

    uint32_t id = node_id_of(key);
    if (id == 0)
        return fail(x, line, "[cluster]: neznámý klíč '%s' (povolené: local, op_timeout_ms, "
                    "node1, node2, ...)", key);
    if (mc->node_count >= KIVFS_MAX_NODES)
        return fail(x, line, "[cluster]: víc než %d uzlů", KIVFS_MAX_NODES);

    struct kivfs_node_addr na;
    na.id = id;
    int r = parse_endpoint(value, x->sync_port, &na.sync);
    if (r == -2)
        return fail(x, line, "[cluster] %s: chybí port a [sync] nemá 'port'", key);
    if (r != 0)
        return fail(x, line, "[cluster] %s: neplatná adresa '%s' (host[:port], IPv6 v [ ])",
                    key, value);

    /* zařadit podle id (insertion sort); duplicitu id ohlídal parser
     * (stejný klíč), kromě "node1" vs. "node01" – to je zakázané výše */
    size_t i = mc->node_count;
    while (i > 0 && mc->nodes[i - 1].id > id) {
        mc->nodes[i] = mc->nodes[i - 1];
        i--;
    }
    mc->nodes[i] = na;
    mc->node_count++;
    return 0;
}

static int own_key(const char *key, const char *value, int line, void *ud)
{
    struct ctx *x = ud;
    (void)value;
    if (in_list(key, g_base_keys) || in_list(key, x->extra))
        return 0;
    return fail(x, line, "[%s]: neznámý klíč '%s'", kivfs_module_name(x->mc->self), key);
}

static int opt_uint(struct ctx *x, const char *sec, const char *key,
                    unsigned long min, unsigned long max, unsigned long *out)
{
    const char *v = kivfs_config_get(x->c, sec, key);
    if (v == NULL)
        return KIVFS_OK;
    if (parse_uint(v, min, max, out) != 0)
        return fail(x, kivfs_config_line(x->c, sec, key),
                    "[%s] %s: '%s' není číslo %lu..%lu", sec, key, v, min, max);
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Veřejné API                                                         */
/* ------------------------------------------------------------------ */

int kivfs_modconf_from(const struct kivfs_config *c, enum kivfs_module self,
                       const char *const *extra_keys,
                       struct kivfs_modconf *out, char *err, size_t errlen)
{
    if (c == NULL || out == NULL || self <= KIVFS_MOD_CLIENT || self >= KIVFS_MOD_COUNT)
        return KIVFS_EINVAL;

    memset(out, 0, sizeof *out);
    out->self          = self;
    out->max_conns     = DEFAULT_MAX_CONNS;
    out->rx_timeout_ms = DEFAULT_RX_TIMEOUT_MS;

    struct ctx x = { c, out, extra_keys, err, errlen, 0 };
    const char *me = kivfs_module_name(self);
    int rc;

    /* 1) jen známé sekce */
    if ((rc = kivfs_config_foreach_section(c, check_section_name, &x)) != 0)
        return rc;

    /* 2) vlastní sekce: existence, neznámé klíče, adresa, volitelné klíče */
    if (kivfs_config_line(c, me, NULL) == 0)
        return fail(&x, 0, "chybí sekce [%s]", me);
    if ((rc = kivfs_config_foreach(c, me, own_key, &x)) != 0)
        return rc;
    if ((rc = parse_module_addr(&x, me, &out->advertise)) != KIVFS_OK)
        return rc;

    const char *bind = kivfs_config_get(c, me, "bind");
    if (bind != NULL) {
        if (!host_ok(bind))
            return fail(&x, kivfs_config_line(c, me, "bind"), "[%s] bind: neplatná adresa", me);
        snprintf(out->bind, sizeof out->bind, "%s", bind);
    }

    unsigned long v;
    v = out->max_conns;
    if ((rc = opt_uint(&x, me, "max_conns", 1, 100000, &v)) != KIVFS_OK) return rc;
    out->max_conns = (uint32_t)v;
    v = 0;
    if ((rc = opt_uint(&x, me, "max_out_conns", 1, 100000, &v)) != KIVFS_OK) return rc;
    out->max_out_conns = (uint32_t)v;
    v = 0;
    if ((rc = opt_uint(&x, me, "max_pending", 1, 100000, &v)) != KIVFS_OK) return rc;
    out->max_pending = (uint32_t)v;
    v = 0;
    if ((rc = opt_uint(&x, me, "connect_timeout_ms", 1, 600000, &v)) != KIVFS_OK) return rc;
    out->connect_timeout_ms = (int)v;
    v = (unsigned long)out->rx_timeout_ms;
    if ((rc = opt_uint(&x, me, "rx_timeout_ms", 0, 600000, &v)) != KIVFS_OK) return rc;
    out->rx_timeout_ms = (int)v;

    /* 3) moduly, které volám */
    for (int m = KIVFS_MOD_AUTH; m < KIVFS_MOD_COUNT; m++) {
        if (!is_dep(self, m))
            continue;
        const char *dn = kivfs_module_name((enum kivfs_module)m);
        if (kivfs_config_line(c, dn, NULL) == 0)
            return fail(&x, 0, "chybí sekce [%s] – modul %s ho volá", dn, me);
        if ((rc = parse_module_addr(&x, dn, &out->deps[m])) != KIVFS_OK)
            return rc;
        out->has_dep[m] = 1;
    }

    /* 4) [cluster]: uzly (výchozí port = [sync] port) a local */
    if (kivfs_config_line(c, "cluster", NULL) == 0)
        return fail(&x, 0, "chybí sekce [cluster]");
    const char *sp = kivfs_config_get(c, "sync", "port");
    if (sp != NULL && parse_uint(sp, 1, 65535, &v) == 0)
        x.sync_port = (int)v;
    if ((rc = kivfs_config_foreach(c, "cluster", cluster_key, &x)) != 0)
        return rc;
    if (out->node_count == 0)
        return fail(&x, kivfs_config_line(c, "cluster", NULL), "[cluster]: žádný uzel (node1 = ...)");

    const char *local = kivfs_config_get(c, "cluster", "local");
    if (local == NULL)
        return fail(&x, kivfs_config_line(c, "cluster", NULL), "[cluster]: chybí povinný klíč 'local'");
    uint32_t lid = node_id_of(local);
    if (lid == 0 || kivfs_modconf_node(out, lid) == NULL)
        return fail(&x, kivfs_config_line(c, "cluster", "local"),
                    "[cluster] local: '%s' není žádný z uzlů nodeN v [cluster]", local);
    out->local_node = lid;

    /* 5) [cluster] op_timeout_ms – časový rámec jedné operace (7.4.3) */
    out->op_timeout_ms = KIVFS_OP_TIMEOUT_DEFAULT_MS;
    const char *ot = kivfs_config_get(c, "cluster", "op_timeout_ms");
    if (ot != NULL) {
        if (parse_uint(ot, KIVFS_OP_TIMEOUT_MIN_MS, KIVFS_OP_TIMEOUT_MAX_MS, &v) != 0)
            return fail(&x, kivfs_config_line(c, "cluster", "op_timeout_ms"),
                        "[cluster] op_timeout_ms: '%s' není číslo %d..%d", ot,
                        KIVFS_OP_TIMEOUT_MIN_MS, KIVFS_OP_TIMEOUT_MAX_MS);
        out->op_timeout_ms = (int)v;
    }
    return KIVFS_OK;
}

void kivfs_modconf_reactor_cfg(const struct kivfs_modconf *mc, struct kivfs_reactor_config *out)
{
    memset(out, 0, sizeof *out);
    out->bind_addr          = mc->bind[0] != '\0' ? mc->bind : NULL;
    out->port               = mc->advertise.port;
    out->backlog            = 128;
    out->max_conns          = mc->max_conns;
    out->max_out_conns      = mc->max_out_conns;
    out->max_pending        = mc->max_pending;
    out->rx_timeout_ms      = mc->rx_timeout_ms;
    out->connect_timeout_ms = mc->connect_timeout_ms;
}

void kivfs_modconf_timeouts(const struct kivfs_modconf *mc, struct kivfs_timeouts *out)
{
    int o = mc->op_timeout_ms;
    out->op           = o;
    out->round        = o * 2 / 16;
    out->sync_vfs     = o * 8 / 16;
    out->vfs_db_read  = o * 2 / 16;
    out->vfs_db_write = o * 5 / 16;
    out->db_submit    = o * 4 / 16;
    out->sync_apply   = o / 16;
}

const struct kivfs_node_addr *kivfs_modconf_node(const struct kivfs_modconf *mc, uint32_t id)
{
    for (size_t i = 0; i < mc->node_count; i++)
        if (mc->nodes[i].id == id)
            return &mc->nodes[i];
    return NULL;
}

int kivfs_modconf_resolve(const struct kivfs_endpoint *ep,
                          struct sockaddr_storage *out, socklen_t *out_len)
{
    return kivfs_sock_resolve(ep->host, ep->port, out, out_len);
}
