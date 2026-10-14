/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * Regression tests for renameat2 with nonzero flags.
 *
 * The two LTP tests for renameat2 are gated on a reported kernel version and never run under
 * Gramine, so the flag behavior is checked directly here: RENAME_NOREPLACE on tmpfs and on the
 * host-backed filesystem, RENAME_EXCHANGE on tmpfs (the filesystem that supports it), and the
 * Linux flag-combination validation results (EINVAL / EOPNOTSUPP / ENOENT / EEXIST).
 *
 * Usage: renameat2 <base-dir> <kind>   where kind is "tmpfs" or "chroot"
 */

#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define RENAME_NOREPLACE (1U << 0)
#define RENAME_EXCHANGE  (1U << 1)
#define RENAME_WHITEOUT  (1U << 2)

static char base[512];
static int is_tmpfs;

static void path_of(char* buf, size_t sz, const char* name) {
    if ((size_t)snprintf(buf, sz, "%s/%s", base, name) >= sz)
        abort();
}

static void write_file(const char* path, const char* content) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        err(1, "open %s", path);
    size_t len = strlen(content);
    if (write(fd, content, len) != (ssize_t)len)
        err(1, "write %s", path);
    close(fd);
}

static void read_file(const char* path, char* buf, size_t sz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        err(1, "open(read) %s", path);
    ssize_t n = read(fd, buf, sz - 1);
    if (n < 0)
        err(1, "read %s", path);
    buf[n] = '\0';
    close(fd);
}

static long do_renameat2(const char* oldp, const char* newp, unsigned int flags) {
    return syscall(SYS_renameat2, AT_FDCWD, oldp, AT_FDCWD, newp, flags);
}

int main(int argc, const char** argv) {
    if (argc != 3)
        errx(1, "usage: renameat2 <base-dir> <tmpfs|chroot>");
    snprintf(base, sizeof(base), "%s", argv[1]);
    if (strcmp(argv[2], "tmpfs") == 0) {
        is_tmpfs = 1;
    } else if (strcmp(argv[2], "chroot") != 0) {
        errx(1, "unknown filesystem kind: %s", argv[2]);
    }

    char a[600], b[600], c[600];
    path_of(a, sizeof(a), "r2_a");
    path_of(b, sizeof(b), "r2_b");
    path_of(c, sizeof(c), "r2_c");

    long r;

    /* 1. RENAME_NOREPLACE with an existing target fails with EEXIST and changes nothing */
    write_file(a, "AAA\n");
    write_file(b, "BBB\n");
    r = do_renameat2(a, b, RENAME_NOREPLACE);
    if (r == 0 || errno != EEXIST)
        errx(1, "NOREPLACE onto existing: want EEXIST, got r=%ld errno=%d", r, errno);
    char buf[32];
    read_file(a, buf, sizeof(buf));
    if (strcmp(buf, "AAA\n") != 0)
        errx(1, "NOREPLACE moved the source anyway");
    read_file(b, buf, sizeof(buf));
    if (strcmp(buf, "BBB\n") != 0)
        errx(1, "NOREPLACE replaced the target anyway");

    /* 2. RENAME_NOREPLACE with no target renames normally */
    unlink(c);
    r = do_renameat2(a, c, RENAME_NOREPLACE);
    if (r)
        err(1, "NOREPLACE onto a free name");
    if (access(a, F_OK) == 0)
        errx(1, "NOREPLACE left the source behind");
    read_file(c, buf, sizeof(buf));
    if (strcmp(buf, "AAA\n") != 0)
        errx(1, "NOREPLACE moved the wrong content");

    if (is_tmpfs) {
        /* 3. RENAME_EXCHANGE swaps two files atomically; inode numbers travel with content */
        write_file(a, "AAA\n");
        write_file(b, "BBB\n");
        struct stat sa, sb;
        if (stat(a, &sa) || stat(b, &sb))
            err(1, "stat before exchange");
        r = do_renameat2(a, b, RENAME_EXCHANGE);
        if (r)
            err(1, "EXCHANGE");
        struct stat na, nb;
        if (stat(a, &na) || stat(b, &nb))
            err(1, "stat after exchange");
        if (na.st_ino != sb.st_ino || nb.st_ino != sa.st_ino)
            errx(1, "EXCHANGE did not swap the inodes");
        read_file(a, buf, sizeof(buf));
        if (strcmp(buf, "BBB\n") != 0)
            errx(1, "EXCHANGE: a does not have the old content of b");
        read_file(b, buf, sizeof(buf));
        if (strcmp(buf, "AAA\n") != 0)
            errx(1, "EXCHANGE: b does not have the old content of a");

        /* 4. RENAME_EXCHANGE with a missing target is ENOENT (nothing is created) */
        unlink(c);
        r = do_renameat2(a, c, RENAME_EXCHANGE);
        if (r == 0 || errno != ENOENT)
            errx(1, "EXCHANGE with missing target: want ENOENT, got r=%ld errno=%d", r, errno);
        if (access(c, F_OK) == 0)
            errx(1, "EXCHANGE created the missing target");

        /* 5. symlinks can be exchanged too */
        char l1[600], l2[600];
        path_of(l1, sizeof(l1), "r2_l1");
        path_of(l2, sizeof(l2), "r2_l2");
        unlink(l1);
        unlink(l2);
        if (symlink("target-one", l1) || symlink("target-two", l2))
            err(1, "symlinks for exchange");
        r = do_renameat2(l1, l2, RENAME_EXCHANGE);
        if (r)
            err(1, "EXCHANGE symlinks");
        char tbuf[64];
        ssize_t n = readlink(l1, tbuf, sizeof(tbuf) - 1);
        if (n != 10 || memcmp(tbuf, "target-two", 10) != 0) {
            tbuf[n < 0 ? 0 : n] = '\0';
            errx(1, "after EXCHANGE l1 points to %s", tbuf);
        }
        n = readlink(l2, tbuf, sizeof(tbuf) - 1);
        if (n != 10 || memcmp(tbuf, "target-one", 10) != 0) {
            tbuf[n < 0 ? 0 : n] = '\0';
            errx(1, "after EXCHANGE l2 points to %s", tbuf);
        }
        unlink(l1);
        unlink(l2);
    } else {
        /* 6. the host-backed filesystem has no exchange hook: honest EOPNOTSUPP */
        write_file(a, "AAA\n");
        r = do_renameat2(a, b, RENAME_EXCHANGE);
        if (r == 0 || errno != EOPNOTSUPP)
            errx(1, "EXCHANGE on chroot: want EOPNOTSUPP, got r=%ld errno=%d", r, errno);
    }

    /* 7. flag validation, same on every filesystem */
    r = do_renameat2(a, b, RENAME_NOREPLACE | RENAME_EXCHANGE);
    if (r == 0 || errno != EINVAL)
        errx(1, "NOREPLACE|EXCHANGE: want EINVAL, got r=%ld errno=%d", r, errno);
    r = do_renameat2(a, b, 8);
    if (r == 0 || errno != EINVAL)
        errx(1, "flags=8: want EINVAL, got r=%ld errno=%d", r, errno);
    r = do_renameat2(a, b, RENAME_WHITEOUT);
    if (r == 0 || errno != EOPNOTSUPP)
        errx(1, "WHITEOUT: want EOPNOTSUPP, got r=%ld errno=%d", r, errno);

    /* 8. flags zero still overwrites (plain rename semantics) */
    r = do_renameat2(a, b, 0);
    if (r)
        err(1, "flags=0 rename");

    unlink(a);
    unlink(b);
    unlink(c);

    puts("TEST OK");
    return 0;
}
