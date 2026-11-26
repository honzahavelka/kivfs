/*
 * Testy mock úložiště metadat (db/backend.h, db/backend_mock.c) – bez sítě:
 * čtení, apply (podmínky, explicitní id/čas, atomicita s archivem,
 * pořadí indexů, INVALID), archiv (log_head, log_read), determinismus.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db/backend.h"
#include "proto/errors.h"
#include "proto/protocol.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

#define DIR(perm)  (KIVFS_MODE_DIR | (perm))
#define FILE_(perm) (KIVFS_MODE_FILE | (perm))

struct listing {
    char names[64][32];
    uint64_t ids[64];
    int n;
    int stop_after;                  /* 0 = nezastavovat */
};

static int collect(void *ud, uint64_t id, uint32_t mode, const char *name)
{
    struct listing *l = ud;
    (void)mode;
    if (l->stop_after && l->n >= l->stop_after)
        return 1;
    if (l->n < 64) {
        snprintf(l->names[l->n], sizeof l->names[0], "%s", name);
        l->ids[l->n] = id;
    }
    l->n++;
    return 0;
}

static int count_only(void *ud, uint64_t id, uint32_t mode, const char *name)
{
    (void)id; (void)mode; (void)name;
    (*(int *)ud)++;
    return 0;
}

/* ---- zápis přes apply: index roste o 1, jen když ho apply spotřebuje ---- */

struct db {
    struct kivfs_dbb *b;
    uint64_t          idx;           /* poslední spotřebovaný index */
};

/* "DB operace" je pro backend neprůhledná – stačí rozpoznatelné bajty. */
static void blob_for(uint64_t index, uint8_t out[16])
{
    memset(out, 0, 16);
    snprintf((char *)out, 16, "op%llu", (unsigned long long)index);
}

static int16_t apply_at(struct db *d, uint64_t index, const struct kivfs_dbb_write *w,
                        struct kivfs_attr *out)
{
    uint8_t blob[16];
    blob_for(index, blob);
    struct kivfs_dbb_logrec r = {
        .index = index, .t = 100 + index, .op = { 1, 7, index },
        .status = 99, .dbop = blob, .dbop_len = sizeof blob };
    int16_t st = d->b->ops->apply(d->b, w, &r, out);
    memset(blob, 0xEE, sizeof blob);              /* backend si musel udělat kopii */
    if (st != KIVFS_STATUS_INTERNAL)
        d->idx = index;
    return st;
}

static int16_t mk(struct db *d, const char *path, uint32_t mode, uint32_t uid, uint32_t gid,
                  uint64_t time_ms, struct kivfs_attr *out)
{
    struct kivfs_dbb_write w = { KIVFS_DBB_W_MKNODE, time_ms, path,
                                 kivfs_db_id_from_index(d->idx + 1), mode, uid, gid };
    return apply_at(d, d->idx + 1, &w, out);
}

static int16_t rm(struct db *d, const char *path, uint64_t time_ms)
{
    struct kivfs_dbb_write w = { KIVFS_DBB_W_RMNODE, time_ms, path, 0, 0, 0, 0 };
    return apply_at(d, d->idx + 1, &w, NULL);
}

static uint64_t head_index(struct db *d)
{
    struct kivfs_dbb_logrec h;
    if (d->b->ops->log_head(d->b, &h) != KIVFS_STATUS_OK)
        return (uint64_t)-1;
    return h.index;
}

struct logcol {
    uint64_t index[64];
    int16_t  status[64];
    int      blob_ok;               /* všechny bajty sedí s indexem */
    int      n, stop_after;
};

static int collect_log(void *ud, const struct kivfs_dbb_logrec *r)
{
    struct logcol *c = ud;
    uint8_t blob[16];
    if (c->stop_after && c->n >= c->stop_after)
        return 1;
    blob_for(r->index, blob);
    if (r->dbop_len != sizeof blob || memcmp(r->dbop, blob, sizeof blob) != 0
        || r->t != 100 + r->index || r->op.node != 1 || r->op.incarnation != 7
        || r->op.seq != r->index)
        c->blob_ok = 0;
    if (c->n < 64) {
        c->index[c->n]  = r->index;
        c->status[c->n] = r->status;
    }
    c->n++;
    return 0;
}

static void test_log_and_order(void)
{
    struct db d = { 0 };
    struct kivfs_dbb_logrec h;
    struct kivfs_attr a, root;
    struct logcol c;

    CHECK(kivfs_dbb_mock_open(&d.b) == KIVFS_OK);
    const struct kivfs_dbb_ops *o = d.b->ops;

    /* prázdný archiv */
    CHECK(o->log_head(d.b, &h) == KIVFS_STATUS_OK && h.index == 0 && h.dbop_len == 0);
    memset(&c, 0, sizeof c);
    CHECK(o->log_read(d.b, 1, 10, collect_log, &c) == KIVFS_STATUS_OK && c.n == 0);

    /* první index musí být 1 */
    struct kivfs_dbb_write w = { KIVFS_DBB_W_MKNODE, 5, "/a", 99, DIR(0755), 1, 1 };
    CHECK(apply_at(&d, 2, &w, NULL) == KIVFS_STATUS_INTERNAL);
    CHECK(head_index(&d) == 0 && o->get(d.b, "/a", &a) == KIVFS_STATUS_NOT_FOUND);

    /* explicitní id a čas od původce; mtime rodiče = čas operace */
    CHECK(apply_at(&d, 1, &w, &a) == KIVFS_STATUS_OK);
    CHECK(a.id == 99 && a.mtime_ms == 5 && a.ctime_ms == 5 && a.uid == 1);
    CHECK(o->get(d.b, "/", &root) == KIVFS_STATUS_OK && root.mtime_ms == 5);
    CHECK(o->log_head(d.b, &h) == KIVFS_STATUS_OK && h.index == 1 && h.status == KIVFS_STATUS_OK);
    CHECK(h.op.seq == 1 && h.t == 101);

    /* neúspěšná podmínka spotřebuje index a archivuje se se svým stavem */
    CHECK(mk(&d, "/a", DIR(0755), 1, 1, 6, NULL) == KIVFS_STATUS_EXISTS);
    CHECK(o->log_head(d.b, &h) == KIVFS_STATUS_OK && h.index == 2 && h.status == KIVFS_STATUS_EXISTS);
    CHECK(o->get(d.b, "/", &root) == KIVFS_STATUS_OK && root.mtime_ms == 5);  /* beze změny */
    CHECK(rm(&d, "/nic", 7) == KIVFS_STATUS_NOT_FOUND && head_index(&d) == 3);

    /* díra i duplicita -> INTERNAL, nic se nezmění */
    w.path = "/b";
    CHECK(apply_at(&d, 5, &w, NULL) == KIVFS_STATUS_INTERNAL);
    CHECK(apply_at(&d, 3, &w, NULL) == KIVFS_STATUS_INTERNAL);
    CHECK(apply_at(&d, 1, &w, NULL) == KIVFS_STATUS_INTERNAL);
    CHECK(head_index(&d) == 3 && o->get(d.b, "/b", &a) == KIVFS_STATUS_NOT_FOUND);

    /* INVALID: MALFORMED, index spotřebován, strom beze změny */
    struct kivfs_dbb_write bad = { KIVFS_DBB_W_INVALID, 8, NULL, 0, 0, 0, 0 };
    CHECK(apply_at(&d, 4, &bad, NULL) == KIVFS_STATUS_MALFORMED);
    CHECK(head_index(&d) == 4);
    CHECK(o->get(d.b, "/", &root) == KIVFS_STATUS_OK && root.mtime_ms == 5);
    /* MKNODE bez cesty = chyba volajícího -> INTERNAL, nic */
    struct kivfs_dbb_write nopath = { KIVFS_DBB_W_MKNODE, 8, NULL, 50, DIR(0755), 1, 1 };
    CHECK(apply_at(&d, 5, &nopath, NULL) == KIVFS_STATUS_INTERNAL && head_index(&d) == 4);

    CHECK(mk(&d, "/b", DIR(0755), 1, 1, 9, &a) == KIVFS_STATUS_OK && a.id == kivfs_db_id_from_index(5));
    CHECK(rm(&d, "/b", 10) == KIVFS_STATUS_OK && head_index(&d) == 6);

    /* log_read: celý archiv, kopie operací, stavy */
    memset(&c, 0, sizeof c);
    c.blob_ok = 1;
    CHECK(o->log_read(d.b, 1, 100, collect_log, &c) == KIVFS_STATUS_OK);
    CHECK(c.n == 6 && c.blob_ok);
    const int16_t want[] = { KIVFS_STATUS_OK, KIVFS_STATUS_EXISTS, KIVFS_STATUS_NOT_FOUND,
                             KIVFS_STATUS_MALFORMED, KIVFS_STATUS_OK, KIVFS_STATUS_OK };
    for (int i = 0; i < 6 && i < c.n; i++)
        CHECK(c.index[i] == (uint64_t)i + 1 && c.status[i] == want[i]);
    /* stránky: od 3 nejvýš 2; from 0 = od 1; za koncem nic; max 0 nic */
    memset(&c, 0, sizeof c);
    CHECK(o->log_read(d.b, 3, 2, collect_log, &c) == KIVFS_STATUS_OK);
    CHECK(c.n == 2 && c.index[0] == 3 && c.index[1] == 4);
    memset(&c, 0, sizeof c);
    CHECK(o->log_read(d.b, 0, 1, collect_log, &c) == KIVFS_STATUS_OK && c.n == 1 && c.index[0] == 1);
    memset(&c, 0, sizeof c);
    CHECK(o->log_read(d.b, 7, 10, collect_log, &c) == KIVFS_STATUS_OK && c.n == 0);
    memset(&c, 0, sizeof c);
    CHECK(o->log_read(d.b, 1, 0, collect_log, &c) == KIVFS_STATUS_OK && c.n == 0);
    /* emit zastaví stránku -> pořád OK */
    memset(&c, 0, sizeof c);
    c.stop_after = 2;
    CHECK(o->log_read(d.b, 1, 10, collect_log, &c) == KIVFS_STATUS_OK && c.n == 2);

    /* dlouhý archiv (realloc pole) */
    for (int i = 0; i < 500; i++) {
        char path[32];
        snprintf(path, sizeof path, "/l%d", i % 50);
        (void)mk(&d, path, DIR(0755), 1, 1, 11, NULL);   /* polovina EXISTS */
    }
    CHECK(head_index(&d) == 506);
    memset(&c, 0, sizeof c);
    c.blob_ok = 1;
    CHECK(o->log_read(d.b, 400, 64, collect_log, &c) == KIVFS_STATUS_OK && c.n == 64 && c.blob_ok);
    CHECK(c.index[0] == 400 && c.index[63] == 463);

    o->close(d.b);
}

/* Dvě repliky, stejná posloupnost operací -> stejný strom (id, časy, stav). */
static void dump_tree(struct kivfs_dbb *b, const char *path, char *out, size_t cap);

struct dump_ctx {
    struct kivfs_dbb *b;
    const char       *dir;
    char             *out;
    size_t            cap;
};

static int dump_entry(void *ud, uint64_t id, uint32_t mode, const char *name)
{
    struct dump_ctx *c = ud;
    char child[256];
    struct kivfs_attr a;
    snprintf(child, sizeof child, "%s%s%s", c->dir, strcmp(c->dir, "/") ? "/" : "", name);
    c->b->ops->get(c->b, child, &a);
    size_t len = strlen(c->out);
    snprintf(c->out + len, c->cap - len, "%s:%llu:%o:%llu:%llu;", child, (unsigned long long)id,
             (unsigned)mode, (unsigned long long)a.mtime_ms, (unsigned long long)a.ctime_ms);
    if ((mode & KIVFS_MODE_TYPE_MASK) == KIVFS_MODE_DIR)
        dump_tree(c->b, child, c->out, c->cap);
    return 0;
}

static void dump_tree(struct kivfs_dbb *b, const char *path, char *out, size_t cap)
{
    struct dump_ctx c = { b, path, out, cap };
    b->ops->list(b, path, dump_entry, &c);
}

static void test_determinism(void)
{
    struct db r1 = { 0 }, r2 = { 0 };
    CHECK(kivfs_dbb_mock_open(&r1.b) == KIVFS_OK);
    CHECK(kivfs_dbb_mock_open(&r2.b) == KIVFS_OK);
    struct db *rs[2] = { &r1, &r2 };
    int16_t res[2][400];

    /* náhodná posloupnost mkdir/rmdir nad malým stromem (souběžné
     * "mkdir /a" ze dvou uzlů = dvě stejné operace za sebou) */
    for (int k = 0; k < 2; k++) {
        srand(42);
        for (int i = 0; i < 400; i++) {
            char path[32];
            int depth = rand() % 3;
            snprintf(path, sizeof path, depth == 0 ? "/d%d" : depth == 1 ? "/d%d/e%d" : "/d%d/e%d/f%d",
                     rand() % 4, rand() % 3, rand() % 2);
            uint64_t t = 1000 + (uint64_t)(rand() % 50);    /* časy původců nemonotónní */
            res[k][i] = rand() % 3 ? mk(rs[k], path, DIR(0755), 1, 1, t, NULL)
                                   : rm(rs[k], path, t);
        }
    }
    int same = 1;
    for (int i = 0; i < 400; i++)
        same &= res[0][i] == res[1][i];
    CHECK(same);
    static char d1[1 << 16], d2[1 << 16];
    d1[0] = d2[0] = '\0';
    dump_tree(r1.b, "/", d1, sizeof d1);
    dump_tree(r2.b, "/", d2, sizeof d2);
    CHECK(strlen(d1) > 0 && strcmp(d1, d2) == 0);
    CHECK(head_index(&r1) == 400 && head_index(&r2) == 400);
    r1.b->ops->close(r1.b);
    r2.b->ops->close(r2.b);
}

int main(void)
{
    struct db d = { 0 };
    struct kivfs_attr a, root;
    struct listing l;

    CHECK(kivfs_dbb_mock_open(&d.b) == KIVFS_OK);
    struct kivfs_dbb *b = d.b;
    const struct kivfs_dbb_ops *o = b->ops;

    /* kořen */
    CHECK(o->get(b, "/", &root) == KIVFS_STATUS_OK);
    CHECK(root.id == KIVFS_DB_ROOT_ID && root.mode == DIR(0777) && root.uid == 0);

    /* mknode + get, id z indexu (index + 1), mtime rodiče se změní */
    CHECK(mk(&d, "/a", DIR(0755), 1000, 1000, 111, &a) == KIVFS_STATUS_OK);
    CHECK(a.id == 2 && a.mode == DIR(0755) && a.uid == 1000 && a.mtime_ms == 111 && a.size == 0);
    CHECK(o->get(b, "/", &root) == KIVFS_STATUS_OK && root.mtime_ms == 111);
    CHECK(o->get(b, "/a", &a) == KIVFS_STATUS_OK && a.id == 2);
    CHECK(mk(&d, "/a/b", DIR(0700), 1000, 1000, 222, &a) == KIVFS_STATUS_OK && a.id == 3);

    /* chyby mknode */
    CHECK(mk(&d, "/a", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_EXISTS);
    CHECK(mk(&d, "/", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_EXISTS);
    CHECK(mk(&d, "/x/y", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_NOT_FOUND);
    CHECK(mk(&d, "/f", FILE_(0644), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
    CHECK(mk(&d, "/f/g", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_NOT_DIR);

    /* get chyby */
    CHECK(o->get(b, "/nic", &a) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->get(b, "/f/g", &a) == KIVFS_STATUS_NOT_DIR);
    CHECK(o->get(b, "/a/bb", &a) == KIVFS_STATUS_NOT_FOUND);   /* prefix "b" != "bb" */
    CHECK(o->get(b, "/a/b", &a) == KIVFS_STATUS_OK);

    /* list: seřazeno podle jména bez ohledu na pořadí vložení */
    CHECK(mk(&d, "/s", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
    const char *order[] = { "c", "a", "bb", "b", "A", "ab" };
    for (size_t i = 0; i < 6; i++) {
        char path[16];
        snprintf(path, sizeof path, "/s/%s", order[i]);
        CHECK(mk(&d, path, DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
    }
    memset(&l, 0, sizeof l);
    CHECK(o->list(b, "/s", collect, &l) == KIVFS_STATUS_OK);
    CHECK(l.n == 6);
    const char *sorted[] = { "A", "a", "ab", "b", "bb", "c" };
    for (int i = 0; i < 6 && i < l.n; i++)
        CHECK(strcmp(l.names[i], sorted[i]) == 0);
    memset(&l, 0, sizeof l);
    CHECK(o->list(b, "/a/b", collect, &l) == KIVFS_STATUS_OK && l.n == 0);
    CHECK(o->list(b, "/f", collect, &l) == KIVFS_STATUS_NOT_DIR);
    CHECK(o->list(b, "/nic", collect, &l) == KIVFS_STATUS_NOT_FOUND);
    memset(&l, 0, sizeof l);
    l.stop_after = 2;
    CHECK(o->list(b, "/s", collect, &l) == KIVFS_STATUS_INTERNAL);   /* emit zastavil */

    /* rmnode */
    CHECK(rm(&d, "/", 5) == KIVFS_STATUS_ACCESS_DENIED);
    CHECK(rm(&d, "/a", 5) == KIVFS_STATUS_NOT_EMPTY);
    CHECK(rm(&d, "/f", 5) == KIVFS_STATUS_NOT_DIR);
    CHECK(rm(&d, "/nic", 5) == KIVFS_STATUS_NOT_FOUND);
    CHECK(rm(&d, "/a/b", 333) == KIVFS_STATUS_OK);
    CHECK(o->get(b, "/a/b", &a) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->get(b, "/a", &a) == KIVFS_STATUS_OK && a.mtime_ms == 333);
    CHECK(rm(&d, "/a", 5) == KIVFS_STATUS_OK);
    CHECK(rm(&d, "/s/b", 5) == KIVFS_STATUS_OK);
    memset(&l, 0, sizeof l);
    o->list(b, "/s", collect, &l);
    CHECK(l.n == 5 && strcmp(l.names[3], "bb") == 0);
    /* id se nerecykluje (index roste i u chyb) */
    CHECK(mk(&d, "/a", DIR(0755), 1, 1, 1, &a) == KIVFS_STATUS_OK && a.id == d.idx + 1 && a.id > 3);

    /* velký adresář: 2000 položek v náhodném pořadí, výpis seřazený */
    CHECK(mk(&d, "/big", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
    srand(1);
    int made = 0;
    for (int i = 0; i < 2000; i++) {
        char path[32];
        snprintf(path, sizeof path, "/big/n%05d", rand() % 100000);
        if (mk(&d, path, FILE_(0644), 1, 1, 1, NULL) == KIVFS_STATUS_OK)
            made++;
    }
    int cnt = 0;
    CHECK(o->list(b, "/big", count_only, &cnt) == KIVFS_STATUS_OK && cnt == made);
    /* každý vytvořený jde najít */
    srand(1);
    int found = 0;
    for (int i = 0; i < 2000; i++) {
        char path[32];
        snprintf(path, sizeof path, "/big/n%05d", rand() % 100000);
        if (o->get(b, path, &a) == KIVFS_STATUS_OK)
            found++;
    }
    CHECK(found == 2000);
    CHECK(head_index(&d) == d.idx);
    o->close(b);                     /* ASan: žádný únik */

    test_log_and_order();
    test_determinism();

    printf("db_mock: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
