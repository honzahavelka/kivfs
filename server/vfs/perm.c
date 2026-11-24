/*
 * KIVFS – kontrola přístupových práv ve VFS (implementace), viz perm.h
 */
#include "vfs/perm.h"
#include "proto/protocol.h"

int kivfs_perm_allowed(const struct kivfs_attr *a, const struct kivfs_ctx *who, unsigned need)
{
    if (who->uid == 0)
        return 1;
    unsigned bits;
    if (who->uid == a->uid)
        bits = (a->mode >> 6) & 7u;
    else if (who->gid == a->gid)
        bits = (a->mode >> 3) & 7u;
    else
        bits = a->mode & 7u;
    return (bits & need) == need;
}

int16_t kivfs_perm_check_dir(const struct kivfs_attr *dir, const struct kivfs_ctx *who,
                             unsigned need)
{
    if ((dir->mode & KIVFS_MODE_TYPE_MASK) != KIVFS_MODE_DIR)
        return KIVFS_STATUS_NOT_DIR;
    if (!kivfs_perm_allowed(dir, who, need))
        return KIVFS_STATUS_ACCESS_DENIED;
    return KIVFS_STATUS_OK;
}
