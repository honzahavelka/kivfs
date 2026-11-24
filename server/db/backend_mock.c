/*
 * KIVFS – mock úložiště metadat: strom v paměti (viz db/backend.h)
 *
 * Uzel = atributy + jméno + seřazené pole potomků (binární hledání,
 * vkládání posunem). Pro desítky až tisíce položek v adresáři stačí;
 * je to náhrada MySQL pro vývoj a testy, ne cílové řešení.
 * Data se při ukončení procesu ztratí.
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

struct mock {
    struct kivfs_dbb base;           /* musí být první (přetypování) */
    struct node     *root;
    uint64_t         next_id;
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
/* Operace                                                             */
/* ------------------------------------------------------------------ */

static int16_t mock_get(struct kivfs_dbb *b, const char *path, struct kivfs_attr *out)
{
    struct node *n;
    int16_t st = lookup((struct mock *)b, path, &n);
    if (st == KIVFS_STATUS_OK)
        *out = n->a;
    return st;
}

static int16_t mock_mknode(struct kivfs_dbb *b, const char *path, uint32_t mode,
                           uint32_t uid, uint32_t gid, uint64_t now_ms, struct kivfs_attr *out)
{
    struct mock *m = (struct mock *)b;
    char parent_path[KIVFS_PATH_MAX + 1];
    const char *name;
    struct node *dir;

    if (kivfs_path_split(path, parent_path, &name) != KIVFS_OK)
        return KIVFS_STATUS_EXISTS;              /* "/" existuje vždy */
    int16_t st = lookup(m, parent_path, &dir);
    if (st != KIVFS_STATUS_OK)
        return st;
    if (!is_dir(dir))
        return KIVFS_STATUS_NOT_DIR;
    long i = find_kid(dir, name, strlen(name));
    if (i >= 0)
        return KIVFS_STATUS_EXISTS;
    size_t pos = (size_t)(-i - 1);

    if (dir->nkids == dir->capkids) {
        size_t cap = dir->capkids ? dir->capkids * 2 : 8;
        struct node **k = realloc(dir->kids, cap * sizeof *k);
        if (k == NULL)
            return KIVFS_STATUS_INTERNAL;
        dir->kids = k;
        dir->capkids = cap;
    }
    struct node *n = calloc(1, sizeof *n);
    if (n == NULL || (n->name = malloc(strlen(name) + 1)) == NULL) {
        free(n);
        return KIVFS_STATUS_INTERNAL;
    }
    strcpy(n->name, name);
    n->parent     = dir;
    n->a.id       = m->next_id++;
    n->a.mode     = mode;
    n->a.uid      = uid;
    n->a.gid      = gid;
    n->a.size     = 0;
    n->a.mtime_ms = now_ms;
    n->a.ctime_ms = now_ms;

    memmove(&dir->kids[pos + 1], &dir->kids[pos], (dir->nkids - pos) * sizeof *dir->kids);
    dir->kids[pos] = n;
    dir->nkids++;
    dir->a.mtime_ms = now_ms;                    /* obsah adresáře se změnil */
    if (out != NULL)
        *out = n->a;
    return KIVFS_STATUS_OK;
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

static int16_t mock_rmnode(struct kivfs_dbb *b, const char *path, uint64_t now_ms)
{
    struct mock *m = (struct mock *)b;
    struct node *n;
    if (strcmp(path, "/") == 0)
        return KIVFS_STATUS_ACCESS_DENIED;
    int16_t st = lookup(m, path, &n);
    if (st != KIVFS_STATUS_OK)
        return st;
    if (!is_dir(n))
        return KIVFS_STATUS_NOT_DIR;
    if (n->nkids > 0)
        return KIVFS_STATUS_NOT_EMPTY;

    struct node *dir = n->parent;
    long i = find_kid(dir, n->name, strlen(n->name));
    size_t pos = (size_t)i;                      /* existuje – našli jsme ho */
    memmove(&dir->kids[pos], &dir->kids[pos + 1], (dir->nkids - pos - 1) * sizeof *dir->kids);
    dir->nkids--;
    dir->a.mtime_ms = now_ms;
    free_node(n);
    return KIVFS_STATUS_OK;
}

static void mock_close(struct kivfs_dbb *b)
{
    struct mock *m = (struct mock *)b;
    if (m == NULL)
        return;
    free_node(m->root);
    free(m);
}

static const struct kivfs_dbb_ops g_mock_ops = {
    .get    = mock_get,
    .mknode = mock_mknode,
    .list   = mock_list,
    .rmnode = mock_rmnode,
    .close  = mock_close,
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
    m->root->a.id   = 1;                         /* viz backend.h */
    m->root->a.mode = KIVFS_MODE_DIR | 0777;
    m->next_id      = 2;
    *out = &m->base;
    return KIVFS_OK;
}
