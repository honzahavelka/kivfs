/*
 * Testy mock úložiště metadat (db/backend.h, db/backend_mock.c) – bez sítě.
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

int main(void)
{
    struct kivfs_dbb *b;
    struct kivfs_attr a, root;
    struct listing l;

    CHECK(kivfs_dbb_mock_open(&b) == KIVFS_OK);
    const struct kivfs_dbb_ops *o = b->ops;

    /* kořen */
    CHECK(o->get(b, "/", &root) == KIVFS_STATUS_OK);
    CHECK(root.id == 1 && root.mode == DIR(0777) && root.uid == 0);

    /* mknode + get, id roste, mtime rodiče se změní */
    CHECK(o->mknode(b, "/a", DIR(0755), 1000, 1000, 111, &a) == KIVFS_STATUS_OK);
    CHECK(a.id == 2 && a.mode == DIR(0755) && a.uid == 1000 && a.mtime_ms == 111 && a.size == 0);
    CHECK(o->get(b, "/", &root) == KIVFS_STATUS_OK && root.mtime_ms == 111);
    CHECK(o->get(b, "/a", &a) == KIVFS_STATUS_OK && a.id == 2);
    CHECK(o->mknode(b, "/a/b", DIR(0700), 1000, 1000, 222, &a) == KIVFS_STATUS_OK && a.id == 3);

    /* chyby mknode */
    CHECK(o->mknode(b, "/a", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_EXISTS);
    CHECK(o->mknode(b, "/", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_EXISTS);
    CHECK(o->mknode(b, "/x/y", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->mknode(b, "/f", FILE_(0644), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
    CHECK(o->mknode(b, "/f/g", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_NOT_DIR);

    /* get chyby */
    CHECK(o->get(b, "/nic", &a) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->get(b, "/f/g", &a) == KIVFS_STATUS_NOT_DIR);
    CHECK(o->get(b, "/a/bb", &a) == KIVFS_STATUS_NOT_FOUND);   /* prefix "b" != "bb" */
    CHECK(o->get(b, "/a/b", &a) == KIVFS_STATUS_OK);

    /* list: seřazeno podle jména bez ohledu na pořadí vložení */
    CHECK(o->mknode(b, "/s", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
    const char *order[] = { "c", "a", "bb", "b", "A", "ab" };
    for (size_t i = 0; i < 6; i++) {
        char path[16];
        snprintf(path, sizeof path, "/s/%s", order[i]);
        CHECK(o->mknode(b, path, DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
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
    CHECK(o->rmnode(b, "/", 5) == KIVFS_STATUS_ACCESS_DENIED);
    CHECK(o->rmnode(b, "/a", 5) == KIVFS_STATUS_NOT_EMPTY);
    CHECK(o->rmnode(b, "/f", 5) == KIVFS_STATUS_NOT_DIR);
    CHECK(o->rmnode(b, "/nic", 5) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->rmnode(b, "/a/b", 333) == KIVFS_STATUS_OK);
    CHECK(o->get(b, "/a/b", &a) == KIVFS_STATUS_NOT_FOUND);
    CHECK(o->get(b, "/a", &a) == KIVFS_STATUS_OK && a.mtime_ms == 333);
    CHECK(o->rmnode(b, "/a", 5) == KIVFS_STATUS_OK);
    CHECK(o->rmnode(b, "/s/b", 5) == KIVFS_STATUS_OK);
    memset(&l, 0, sizeof l);
    o->list(b, "/s", collect, &l);
    CHECK(l.n == 5 && strcmp(l.names[3], "bb") == 0);
    /* id se nerecykluje */
    CHECK(o->mknode(b, "/a", DIR(0755), 1, 1, 1, &a) == KIVFS_STATUS_OK && a.id > 3);

    /* velký adresář: 2000 položek v náhodném pořadí, výpis seřazený */
    CHECK(o->mknode(b, "/big", DIR(0755), 1, 1, 1, NULL) == KIVFS_STATUS_OK);
    srand(1);
    int made = 0;
    for (int i = 0; i < 2000; i++) {
        char path[32];
        snprintf(path, sizeof path, "/big/n%05d", rand() % 100000);
        if (o->mknode(b, path, FILE_(0644), 1, 1, 1, NULL) == KIVFS_STATUS_OK)
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

    o->close(b);                     /* ASan: žádný únik */
    printf("db_mock: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
