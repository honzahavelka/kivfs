/*
 * Testy kontroly práv ve VFS (vfs/perm.h) – bez sítě.
 */
#include <stdio.h>

#include "proto/protocol.h"
#include "vfs/perm.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

#define R KIVFS_PERM_R
#define W KIVFS_PERM_W
#define X KIVFS_PERM_X

int main(void)
{
    struct kivfs_attr d = { .id = 2, .mode = KIVFS_MODE_DIR | 0750, .uid = 1000, .gid = 100 };
    struct kivfs_ctx owner = { 1000, 100 }, group = { 2000, 100 }, other = { 3000, 300 },
                     root = { 0, 0 }, owner_other_gid = { 1000, 999 };

    /* vlastník rwx, skupina r-x, ostatní --- */
    CHECK(kivfs_perm_allowed(&d, &owner, R | W | X));
    CHECK(kivfs_perm_allowed(&d, &group, R | X));
    CHECK(!kivfs_perm_allowed(&d, &group, W));
    CHECK(!kivfs_perm_allowed(&d, &group, W | X));
    CHECK(!kivfs_perm_allowed(&d, &other, R));
    CHECK(kivfs_perm_allowed(&d, &other, 0));          /* nic nepotřebuje */
    CHECK(kivfs_perm_allowed(&d, &root, R | W | X));
    CHECK(kivfs_perm_allowed(&d, &owner_other_gid, W)); /* rozhoduje uid */

    /* vlastník, který má MÉNĚ než skupina: platí jen bity vlastníka (POSIX) */
    struct kivfs_attr odd = { .mode = KIVFS_MODE_DIR | 0070, .uid = 1000, .gid = 100 };
    CHECK(!kivfs_perm_allowed(&odd, &owner, R));
    CHECK(kivfs_perm_allowed(&odd, &group, R | W | X));

    /* kontrola adresáře */
    CHECK(kivfs_perm_check_dir(&d, &owner, W | X) == KIVFS_STATUS_OK);
    CHECK(kivfs_perm_check_dir(&d, &group, W | X) == KIVFS_STATUS_ACCESS_DENIED);
    CHECK(kivfs_perm_check_dir(&d, &group, R) == KIVFS_STATUS_OK);
    struct kivfs_attr f = { .mode = KIVFS_MODE_FILE | 0777, .uid = 1000 };
    CHECK(kivfs_perm_check_dir(&f, &owner, R) == KIVFS_STATUS_NOT_DIR);
    CHECK(kivfs_perm_check_dir(&f, &root, R) == KIVFS_STATUS_NOT_DIR);   /* ani root */

    /* kořen 0777 uid 0: mkdir smí každý */
    struct kivfs_attr rootdir = { .id = 1, .mode = KIVFS_MODE_DIR | 0777 };
    CHECK(kivfs_perm_check_dir(&rootdir, &other, W | X) == KIVFS_STATUS_OK);

    printf("vfs_perm: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
