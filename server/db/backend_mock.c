/*
 * KIVFS – mock úložiště metadat: strom + archiv v paměti (viz db/backend.h)
 *
 * Uzel = atributy + jméno + seřazené pole potomků (binární hledání,
 * vkládání posunem). Pro desítky až tisíce položek v adresáři stačí;
 * je to náhrada MySQL pro vývoj a testy, ne cílové řešení.
 * Archiv (MESSAGE_LOGS) = pole záznamů, log[i].index == i + 1.
 * Data i archiv se při ukončení procesu ztratí.
 *
 * Atomicita apply: nejdřív se připraví vše, co může selhat (místo
 * v archivu, kopie operace, nový uzel, místo v poli potomků), teprve pak
 * se mění strom a archiv – selhání = INTERNAL bez jakékoli změny.
 */
#include "db/backend.h"
#include "proto/errors.h"
#include "proto/protocol.h"

#include <stdlib.h>
#include <string.h>

struct node {
    struct kivfs_attr a;
    char             *name;          /* NULL u kořene */
    struct node      *parent;
    struct node     **kids;          /* seřazeno podle strcmp(name) */
    size_t            nkids, capkids;
};

struct logent {
    uint64_t           t;
    struct kivfs_op_id op;
    int16_t            status;
    uint8_t           *dbop;
    uint32_t           dbop_len;
};

struct mock {
    struct kivfs_dbb base;           /* musí být první (přetypování) */
    struct node     *root;
    struct logent   *log;            /* log[i] má index i + 1 */
    size_t           nlog, caplog;
};

static int is_dir(const struct node *n)
{
    return (n->a.mode & KIVFS_MODE_TYPE_MASK) == KIVFS_MODE_DIR;
}

/* Binární hledání potomka podle jména (délka nlen, bez '\0').
 * Vrací index nalezeného, nebo -(místo_pro_vložení + 1). */
static long find_kid(const struct node *dir, const char *name, size_t nlen)
{
    size_t lo = 0, hi = dir->nkids;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *k = dir->kids[mid]->name;
        int c = strncmp(k, name, nlen);
        if (c == 0 && k[nlen] != '\0')
            c = 1;                   /* k je delší se stejným prefixem */
        if (c == 0)
            return (long)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -(long)lo - 1;
}

/* Najde uzel podle (už zkontrolované) cesty. */
static int16_t lookup(struct mock *m, const char *path, struct node **out)
{
    struct node *n = m->root;
    const char *p = path + 1;        /* za úvodním '/' */
    while (*p != '\0') {
        if (!is_dir(n))
            return KIVFS_STATUS_NOT_DIR;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        long i = find_kid(n, p, len);
        if (i < 0)
            return KIVFS_STATUS_NOT_FOUND;
        n = n->kids[i];
        p += len;
        if (*p == '/')
            p++;
    }
    *out = n;
    return KIVFS_STATUS_OK;
}

static void free_node(struct node *n)
{
    for (size_t i = 0; i < n->nkids; i++)
        free_node(n->kids[i]);
    free(n->kids);
    free(n->name);
    free(n);
}

/* ------------------------------------------------------------------ */
/* Čtení                                                               */
/* ------------------------------------------------------------------ */

static int16_t mock_get(struct kivfs_dbb *b, const char *path, struct kivfs_attr *out)
{
    struct node *n;
    int16_t st = lookup((struct mock *)b, path, &n);
    if (st == KIVFS_STATUS_OK)
        *out = n->a;
    return st;
}

static int16_t mock_list(struct kivfs_dbb *b, const char *path,
                         int (*emit)(void *ud, uint64_t id, uint32_t mode, const char *name),
                         void *ud)
{
    struct node *n;
    int16_t st = lookup((struct mock *)b, path, &n);
    if (st != KIVFS_STATUS_OK)
        return st;
    if (!is_dir(n))
        return KIVFS_STATUS_NOT_DIR;
    for (size_t i = 0; i < n->nkids; i++)
        if (emit(ud, n->kids[i]->a.id, n->kids[i]->a.mode, n->kids[i]->name) != 0)
            return KIVFS_STATUS_INTERNAL;
    return KIVFS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Zápis: příprava (může selhat, nic nemění) + provedení (nesmí selhat) */
/* ------------------------------------------------------------------ */

/* Připravená změna stromu. */
struct prep {
    struct node *dir;                /* rodič */
    struct node *n;                  /* MKNODE: nový uzel; RMNODE: mazaný */
    size_t       pos;                /* pozice v dir->kids */
};

/* Zajistí místo pro jednoho dalšího potomka (realloc nic nemění na obsahu). */
static int reserve_kid(struct node *dir)
{
    if (dir->nkids < dir->capkids)
        return 0;
    size_t cap = dir->capkids ? dir->capkids * 2 : 8;
    struct node **k = realloc(dir->kids, cap * sizeof *k);
    if (k == NULL)
        return -1;
    dir->kids = k;
    dir->capkids = cap;
    return 0;
}

/* MKNODE: podmínky + alokace. OK -> p vyplněno; jinak stav podmínky
 * (INTERNAL = nedostatek paměti). */
static int16_t prep_mknode(struct mock *m, const struct kivfs_dbb_write *w, struct prep *p)
{
    char parent_path[KIVFS_PATH_MAX + 1];
    const char *name;

    if (kivfs_path_split(w->path, parent_path, &name) != KIVFS_OK)
        return KIVFS_STATUS_EXISTS;              /* "/" existuje vždy */
    int16_t st = lookup(m, parent_path, &p->dir);
    if (st != KIVFS_STATUS_OK)
        return st;
    if (!is_dir(p->dir))
        return KIVFS_STATUS_NOT_DIR;
    long i = find_kid(p->dir, name, strlen(name));
    if (i >= 0)
        return KIVFS_STATUS_EXISTS;
    p->pos = (size_t)(-i - 1);

    if (reserve_kid(p->dir) != 0)
        return KIVFS_STATUS_INTERNAL;
    struct node *n = calloc(1, sizeof *n);
    if (n == NULL || (n->name = malloc(strlen(name) + 1)) == NULL) {
        free(n);
        return KIVFS_STATUS_INTERNAL;
    }
    strcpy(n->name, name);
    n->parent     = p->dir;
    n->a.id       = w->new_id;
    n->a.mode     = w->mode;
    n->a.uid      = w->uid;
    n->a.gid      = w->gid;
    n->a.size     = 0;
    n->a.mtime_ms = w->time_ms;
    n->a.ctime_ms = w->time_ms;
    p->n = n;
    return KIVFS_STATUS_OK;
}

static void do_mknode(const struct prep *p, uint64_t time_ms)
{
    struct node *dir = p->dir;
    memmove(&dir->kids[p->pos + 1], &dir->kids[p->pos], (dir->nkids - p->pos) * sizeof *dir->kids);
    dir->kids[p->pos] = p->n;
    dir->nkids++;
    dir->a.mtime_ms = time_ms;                   /* obsah adresáře se změnil */
}

/* RMNODE: jen podmínky (mazání nic nealokuje). */
static int16_t prep_rmnode(struct mock *m, const struct kivfs_dbb_write *w, struct prep *p)
{
    if (strcmp(w->path, "/") == 0)
        return KIVFS_STATUS_ACCESS_DENIED;
    int16_t st = lookup(m, w->path, &p->n);
    if (st != KIVFS_STATUS_OK)
        return st;
    if (!is_dir(p->n))
        return KIVFS_STATUS_NOT_DIR;
    if (p->n->nkids > 0)
        return KIVFS_STATUS_NOT_EMPTY;
    p->dir = p->n->parent;
    p->pos = (size_t)find_kid(p->dir, p->n->name, strlen(p->n->name));   /* existuje */
    return KIVFS_STATUS_OK;
}

static void do_rmnode(const struct prep *p, uint64_t time_ms)
{
    struct node *dir = p->dir;
    memmove(&dir->kids[p->pos], &dir->kids[p->pos + 1],
            (dir->nkids - p->pos - 1) * sizeof *dir->kids);
    dir->nkids--;
    dir->a.mtime_ms = time_ms;
    free_node(p->n);
}

static int16_t mock_apply(struct kivfs_dbb *b, const struct kivfs_dbb_write *w,
                          const struct kivfs_dbb_logrec *log, struct kivfs_attr *out)
{
    struct mock *m = (struct mock *)b;
    struct prep p = { 0 };
    int16_t st;

    if (log->index != (uint64_t)m->nlog + 1)
        return KIVFS_STATUS_INTERNAL;            /* díra / duplicita */
    if ((w->kind == KIVFS_DBB_W_MKNODE || w->kind == KIVFS_DBB_W_RMNODE) && w->path == NULL)
        return KIVFS_STATUS_INTERNAL;            /* chyba volajícího */

    /* 1) archiv: místo + kopie operace */
    if (m->nlog == m->caplog) {
        size_t cap = m->caplog ? m->caplog * 2 : 64;
        struct logent *l = realloc(m->log, cap * sizeof *l);
        if (l == NULL)
            return KIVFS_STATUS_INTERNAL;
        m->log = l;
        m->caplog = cap;
    }
    uint8_t *copy = malloc(log->dbop_len ? log->dbop_len : 1);
    if (copy == NULL)
        return KIVFS_STATUS_INTERNAL;
    if (log->dbop_len)
        memcpy(copy, log->dbop, log->dbop_len);

    /* 2) podmínky + příprava stromu */
    switch (w->kind) {
    case KIVFS_DBB_W_MKNODE: st = prep_mknode(m, w, &p); break;
    case KIVFS_DBB_W_RMNODE: st = prep_rmnode(m, w, &p); break;
    default:                 st = KIVFS_STATUS_MALFORMED; break;
    }
    if (st == KIVFS_STATUS_INTERNAL) {
        free(copy);
        return st;
    }

    /* 3) provedení – odtud nic nesmí selhat */
    if (st == KIVFS_STATUS_OK) {
        if (w->kind == KIVFS_DBB_W_MKNODE) {
            do_mknode(&p, w->time_ms);
            if (out != NULL)
                *out = p.n->a;
        } else {
            do_rmnode(&p, w->time_ms);
        }
    }
    m->log[m->nlog++] = (struct logent){ log->t, log->op, st, copy, log->dbop_len };
    return st;
}

/* ------------------------------------------------------------------ */
/* Archiv                                                              */
/* ------------------------------------------------------------------ */

static void fill_rec(const struct mock *m, size_t i, struct kivfs_dbb_logrec *r)
{
    const struct logent *e = &m->log[i];
    r->index    = (uint64_t)i + 1;
    r->t        = e->t;
    r->op       = e->op;
    r->status   = e->status;
    r->dbop     = e->dbop;
    r->dbop_len = e->dbop_len;
}

static int16_t mock_log_head(struct kivfs_dbb *b, struct kivfs_dbb_logrec *last)
{
    struct mock *m = (struct mock *)b;
    memset(last, 0, sizeof *last);
    if (m->nlog > 0)
        fill_rec(m, m->nlog - 1, last);
    return KIVFS_STATUS_OK;
}

static int16_t mock_log_read(struct kivfs_dbb *b, uint64_t from, uint32_t max,
                             int (*emit)(void *ud, const struct kivfs_dbb_logrec *r),
                             void *ud)
{
    struct mock *m = (struct mock *)b;
    struct kivfs_dbb_logrec r;
    if (from == 0)
        from = 1;
    for (uint64_t i = from - 1; i < m->nlog && max > 0; i++, max--) {
        fill_rec(m, (size_t)i, &r);
        if (emit(ud, &r) != 0)
            break;
    }
    return KIVFS_STATUS_OK;
}

static void mock_close(struct kivfs_dbb *b)
{
    struct mock *m = (struct mock *)b;
    if (m == NULL)
        return;
    free_node(m->root);
    for (size_t i = 0; i < m->nlog; i++)
        free(m->log[i].dbop);
    free(m->log);
    free(m);
}

static const struct kivfs_dbb_ops g_mock_ops = {
    .get      = mock_get,
    .list     = mock_list,
    .apply    = mock_apply,
    .log_head = mock_log_head,
    .log_read = mock_log_read,
    .close    = mock_close,
};

int kivfs_dbb_mock_open(struct kivfs_dbb **out)
{
    struct mock *m = calloc(1, sizeof *m);
    if (m == NULL)
        return KIVFS_ENOMEM;
    m->root = calloc(1, sizeof *m->root);
    if (m->root == NULL) {
        free(m);
        return KIVFS_ENOMEM;
    }
    m->base.ops     = &g_mock_ops;
    m->root->a.id   = KIVFS_DB_ROOT_ID;          /* viz backend.h */
    m->root->a.mode = KIVFS_MODE_DIR | 0777;
    *out = &m->base;
    return KIVFS_OK;
}
