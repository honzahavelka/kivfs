/*
 * KIVFS – úložiště bloků v adresáři na disku (viz store.h).
 *
 * Jen POSIX (open/fsync/rename/flock), takže test běží i na macOS.
 */
/* flock() a dirfd() nejsou v čistém POSIX: na Linuxu je zpřístupní
 * _DEFAULT_SOURCE, na macOS by _POSIX_C_SOURCE naopak skrylo flock. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _DEFAULT_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fs/store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "proto/errors.h"
#include "proto/protocol.h"

/* Nejdelší cesta uvnitř rootu: "/blocks/ab/cd/" + 64 hex = 78 znaků,
 * "/tmp/" + 64 hex + "." + číslo, "/bad/" + totéž. */
#define ROOT_MAX   3900
#define PATH_BUF   (ROOT_MAX + 128)

struct dir_store {
    struct kivfs_store base;          /* musí být první */
    char               root[ROOT_MAX + 1];
    int                fsync;
    int                lock_fd;       /* drží flock po celou dobu života */
    uint64_t           seq;           /* jména dočasných souborů */
    kivfs_store_log_fn log;
    void              *log_ud;
};

static void slog(struct dir_store *s, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    if (s->log == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    s->log(s->log_ud, msg);
}

static void block_path(const struct dir_store *s, const char *hex, char *out)
{
    snprintf(out, PATH_BUF, "%s/blocks/%.2s/%.2s/%s", s->root, hex, hex + 2, hex);
}

static int fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

/* mkdir, existující adresář je v pořádku. *created = 1, když vznikl. */
static int mkdir_ok(const char *path, int *created)
{
    if (mkdir(path, 0700) == 0) {
        if (created) *created = 1;
        return 0;
    }
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
    }
    return -1;
}

static int16_t errno_status(int e)
{
    if (e == ENOSPC || e == EDQUOT || e == EFBIG)
        return KIVFS_STATUS_NO_SPACE;
    return KIVFS_STATUS_INTERNAL;
}

/* write celý buffer (EINTR, krátké zápisy) */
static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int16_t d_put(struct kivfs_store *bs, const struct kivfs_digest *h,
                     const void *data, uint32_t len)
{
    struct dir_store *s = (struct dir_store *)bs;
    struct kivfs_digest d;
    char hex[KIVFS_DIGEST_HEX_LEN + 1];
    char dst[PATH_BUF], tmp[PATH_BUF], dir[PATH_BUF];
    struct stat st;

    if (len == 0 || len > KIVFS_FS_BLOCK_MAX || data == NULL)
        return KIVFS_STATUS_MALFORMED;
    kivfs_sha256(data, len, &d);
    if (!kivfs_digest_eq(&d, h))
        return KIVFS_STATUS_MALFORMED;

    kivfs_digest_hex(h, hex);
    block_path(s, hex, dst);
    if (stat(dst, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == len)
        return KIVFS_STATUS_OK;               /* už je – idempotence */

    /* 1) data do tmp/ */
    int fd = -1;
    for (int tries = 0; tries < 8 && fd < 0; tries++) {
        snprintf(tmp, sizeof tmp, "%s/tmp/%s.%llu", s->root, hex,
                 (unsigned long long)++s->seq);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0) {
        int e = errno;
        slog(s, "store: open %s: %s", tmp, strerror(e));
        return errno_status(e);
    }
    if (write_all(fd, data, len) != 0 || (s->fsync && fsync(fd) != 0)) {
        int e = errno;
        slog(s, "store: zápis %s: %s", tmp, strerror(e));
        close(fd);
        unlink(tmp);
        return errno_status(e);
    }
    if (close(fd) != 0) {
        int e = errno;
        slog(s, "store: close %s: %s", tmp, strerror(e));
        unlink(tmp);
        return errno_status(e);
    }

    /* 2) adresáře ab/cd (nově vzniklý adresář musí být trvalý i v rodiči) */
    int made_ab = 0, made_cd = 0;
    char ab[PATH_BUF];
    snprintf(ab, sizeof ab, "%s/blocks/%.2s", s->root, hex);
    snprintf(dir, sizeof dir, "%s/blocks/%.2s/%.2s", s->root, hex, hex + 2);
    if (mkdir_ok(ab, &made_ab) != 0 || mkdir_ok(dir, &made_cd) != 0) {
        int e = errno;
        slog(s, "store: mkdir %s: %s", dir, strerror(e));
        unlink(tmp);
        return errno_status(e);
    }
    if (s->fsync) {
        char blocks[PATH_BUF];
        snprintf(blocks, sizeof blocks, "%s/blocks", s->root);
        if ((made_ab && fsync_dir(blocks) != 0) || (made_cd && fsync_dir(ab) != 0)) {
            int e = errno;
            slog(s, "store: fsync adresáře: %s", strerror(e));
            unlink(tmp);
            return errno_status(e);
        }
    }

    /* 3) atomické přejmenování na jméno otisku (případný poškozený blok
     *    s jinou délkou se tím nahradí) */
    if (rename(tmp, dst) != 0) {
        int e = errno;
        slog(s, "store: rename %s: %s", dst, strerror(e));
        unlink(tmp);
        return errno_status(e);
    }
    if (s->fsync && fsync_dir(dir) != 0) {
        /* blok už pod svým jménem je, jen nemusí přežít pád stroje –
         * nahlásit chybu, ať se nepotvrdí; opakovaný put ho najde */
        slog(s, "store: fsync %s: %s", dir, strerror(errno));
        return KIVFS_STATUS_INTERNAL;
    }
    return KIVFS_STATUS_OK;
}

/* Poškozený blok: přesunout do bad/ (karanténa), zalogovat. */
static void quarantine(struct dir_store *s, const char *hex, const char *path, const char *why)
{
    char bad[PATH_BUF];
    snprintf(bad, sizeof bad, "%s/bad/%s.%llu", s->root, hex, (unsigned long long)++s->seq);
    if (rename(path, bad) != 0)
        slog(s, "store: poškozený blok %s (%s), karanténa selhala: %s", hex, why, strerror(errno));
    else
        slog(s, "store: poškozený blok %s (%s) -> %s", hex, why, bad);
}

static int16_t d_get(struct kivfs_store *bs, const struct kivfs_digest *h,
                     void *buf, uint32_t cap, uint32_t *len, int verify)
{
    struct dir_store *s = (struct dir_store *)bs;
    char hex[KIVFS_DIGEST_HEX_LEN + 1], path[PATH_BUF];
    struct stat st;

    kivfs_digest_hex(h, hex);
    block_path(s, hex, path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return KIVFS_STATUS_NOT_FOUND;
        slog(s, "store: open %s: %s", path, strerror(errno));
        return KIVFS_STATUS_INTERNAL;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        slog(s, "store: fstat %s selhal nebo není soubor", path);
        close(fd);
        return KIVFS_STATUS_INTERNAL;
    }
    if (st.st_size == 0 || (uint64_t)st.st_size > KIVFS_FS_BLOCK_MAX) {
        close(fd);
        quarantine(s, hex, path, "nemožná délka");
        return KIVFS_STATUS_NOT_FOUND;
    }
    if ((uint64_t)st.st_size > cap) {
        slog(s, "store: blok %s má %lld B, buffer jen %u B", hex,
             (long long)st.st_size, cap);
        close(fd);
        return KIVFS_STATUS_INTERNAL;
    }

    size_t want = (size_t)st.st_size, got = 0;
    uint8_t *p = buf;
    while (got < want) {
        ssize_t r = read(fd, p + got, want - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            slog(s, "store: read %s: %s", path, strerror(errno));
            close(fd);
            return KIVFS_STATUS_INTERNAL;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    if (got != want) {
        slog(s, "store: %s se během čtení zkrátil", path);
        return KIVFS_STATUS_INTERNAL;
    }

    if (verify) {
        struct kivfs_digest d;
        kivfs_sha256(buf, got, &d);
        if (!kivfs_digest_eq(&d, h)) {
            quarantine(s, hex, path, "nesedí otisk");
            return KIVFS_STATUS_NOT_FOUND;
        }
    }
    *len = (uint32_t)got;
    return KIVFS_STATUS_OK;
}

static int16_t d_has(struct kivfs_store *bs, const struct kivfs_digest *h, uint32_t *len)
{
    struct dir_store *s = (struct dir_store *)bs;
    char hex[KIVFS_DIGEST_HEX_LEN + 1], path[PATH_BUF];
    struct stat st;

    kivfs_digest_hex(h, hex);
    block_path(s, hex, path);
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return KIVFS_STATUS_NOT_FOUND;
        slog(s, "store: stat %s: %s", path, strerror(errno));
        return KIVFS_STATUS_INTERNAL;
    }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size > KIVFS_FS_BLOCK_MAX) {
        slog(s, "store: %s není platný blok", path);
        return KIVFS_STATUS_INTERNAL;
    }
    if (len != NULL)
        *len = (uint32_t)st.st_size;
    return KIVFS_STATUS_OK;
}

static int16_t d_remove(struct kivfs_store *bs, const struct kivfs_digest *h)
{
    struct dir_store *s = (struct dir_store *)bs;
    char hex[KIVFS_DIGEST_HEX_LEN + 1], path[PATH_BUF];

    kivfs_digest_hex(h, hex);
    block_path(s, hex, path);
    if (unlink(path) != 0) {
        if (errno == ENOENT)
            return KIVFS_STATUS_NOT_FOUND;
        slog(s, "store: unlink %s: %s", path, strerror(errno));
        return KIVFS_STATUS_INTERNAL;
    }
    return KIVFS_STATUS_OK;
}

static void d_close(struct kivfs_store *bs)
{
    struct dir_store *s = (struct dir_store *)bs;
    if (s == NULL)
        return;
    if (s->lock_fd >= 0)
        close(s->lock_fd);                     /* uvolní flock */
    free(s);
}

static const struct kivfs_store_ops dir_ops = {
    .put = d_put, .get = d_get, .has = d_has, .remove = d_remove, .close = d_close
};

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err != NULL && errlen > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return KIVFS_ECONFIG;
}

/* Smaže zbytky rozpracovaných zápisů v tmp/ (pád uprostřed put). */
static int clean_tmp(struct dir_store *s, char *err, size_t errlen)
{
    char path[PATH_BUF];
    snprintf(path, sizeof path, "%s/tmp", s->root);
    DIR *dir = opendir(path);
    if (dir == NULL)
        return fail(err, errlen, "fs: opendir %s: %s", path, strerror(errno));
    struct dirent *de;
    int removed = 0;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (unlinkat(dirfd(dir), de->d_name, 0) == 0)
            removed++;
        else
            slog(s, "store: nelze smazat %s/%s: %s", path, de->d_name, strerror(errno));
    }
    closedir(dir);
    if (removed > 0)
        slog(s, "store: smazáno %d rozpracovaných zápisů v %s", removed, path);
    return KIVFS_OK;
}

int kivfs_store_dir_open(const struct kivfs_store_dir_cfg *cfg,
                         struct kivfs_store **out, char *err, size_t errlen)
{
    if (cfg == NULL || cfg->root == NULL || cfg->root[0] == '\0')
        return fail(err, errlen, "fs: chybí root úložiště");
    if (cfg->root[0] != '/')
        return fail(err, errlen, "fs: root musí být absolutní cesta: %s", cfg->root);
    size_t rl = strlen(cfg->root);
    while (rl > 1 && cfg->root[rl - 1] == '/')
        rl--;                                  /* "/data/" -> "/data" */
    if (rl > ROOT_MAX)
        return fail(err, errlen, "fs: root je delší než %d znaků", ROOT_MAX);

    struct dir_store *s = calloc(1, sizeof *s);
    if (s == NULL)
        return KIVFS_ENOMEM;
    memcpy(s->root, cfg->root, rl);
    s->root[rl] = '\0';
    s->fsync = cfg->fsync;
    s->lock_fd = -1;
    s->log = cfg->log;
    s->log_ud = cfg->log_ud;

    /* root (i s rodiči – typicky prázdný Docker volume) a podadresáře */
    char path[PATH_BUF];
    for (size_t i = 1; i <= rl; i++) {
        if (s->root[i] == '/' || s->root[i] == '\0') {
            memcpy(path, s->root, i);
            path[i] = '\0';
            if (mkdir(path, 0700) != 0 && errno != EEXIST) {
                int e = errno;
                free(s);
                return fail(err, errlen, "fs: mkdir %s: %s", path, strerror(e));
            }
        }
    }
    static const char *const sub[] = { "blocks", "tmp", "bad" };
    for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", s->root, sub[i]);
        if (mkdir_ok(path, NULL) != 0) {
            int e = errno;
            free(s);
            return fail(err, errlen, "fs: mkdir %s: %s", path, strerror(e));
        }
    }

    /* zámek: stejný root nesmí používat dva procesy (3 uzly na jednom
     * stroji se stejnou konfigurací by si jinak mazaly tmp/) */
    snprintf(path, sizeof path, "%s/LOCK", s->root);
    s->lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (s->lock_fd < 0) {
        int e = errno;
        free(s);
        return fail(err, errlen, "fs: %s: %s", path, strerror(e));
    }
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        close(s->lock_fd);
        free(s);
        if (e == EWOULDBLOCK)
            return fail(err, errlen, "fs: root %.*s už používá jiný proces",
                        (int)rl, cfg->root);
        return fail(err, errlen, "fs: flock %s: %s", path, strerror(e));
    }

    int rc = clean_tmp(s, err, errlen);
    if (rc != KIVFS_OK) {
        d_close(&s->base);
        return rc;
    }
    s->base.ops = &dir_ops;
    *out = &s->base;
    return KIVFS_OK;
}
