/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * Regression tests for renameat2 with nonzero flags.
 *
 * The two LTP tests for renameat2 are gated on a reported kernel version and never run under
 * Saguaro, so the flag behavior is checked directly here: RENAME_NOREPLACE on tmpfs and on the
 * host-backed filesystem, RENAME_EXCHANGE on tmpfs and on the host-backed filesystem, and the
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

        /* 5b. directories: contents travel with the directory, a directory and a file can trade
         *     places across parents, the cwd moves with its
         *     directory, and a directory cannot be exchanged with its own descendant */
        char d1[600], d2[600], p[600], pf[600], tmp[700];
        path_of(d1, sizeof(d1), "r2_d1");
        path_of(d2, sizeof(d2), "r2_d2");
        path_of(p, sizeof(p), "r2_p");
        path_of(pf, sizeof(pf), "r2_p/f");
        if (mkdir(d1, 0755) || mkdir(d2, 0755) || mkdir(p, 0755))
            err(1, "mkdir for directory exchange");
        snprintf(tmp, sizeof(tmp), "%s/x", d1);
        write_file(tmp, "XXX\n");
        snprintf(tmp, sizeof(tmp), "%s/y", d2);
        write_file(tmp, "YYY\n");
        write_file(pf, "FFF\n");
        struct stat sd1, sd2;
        if (stat(d1, &sd1) || stat(d2, &sd2))
            err(1, "stat dirs");
        r = do_renameat2(d1, d2, RENAME_EXCHANGE);
        if (r)
            err(1, "EXCHANGE directories");
        if (stat(d1, &na) || stat(d2, &nb))
            err(1, "stat dirs after exchange");
        if (na.st_ino != sd2.st_ino || nb.st_ino != sd1.st_ino)
            errx(1, "directory EXCHANGE did not swap the inodes");
        snprintf(tmp, sizeof(tmp), "%s/y", d1);
        read_file(tmp, buf, sizeof(buf));
        if (strcmp(buf, "YYY\n") != 0)
            errx(1, "directory EXCHANGE: d1/y has the wrong content");
        snprintf(tmp, sizeof(tmp), "%s/x", d2);
        read_file(tmp, buf, sizeof(buf));
        if (strcmp(buf, "XXX\n") != 0)
            errx(1, "directory EXCHANGE: d2/x has the wrong content");
        snprintf(tmp, sizeof(tmp), "%s/x", d1);
        if (access(tmp, F_OK) == 0)
            errx(1, "directory EXCHANGE: d1/x still visible");

        struct stat sp;
        if (stat(p, &sp))
            err(1, "stat p");
        r = do_renameat2(d1, pf, RENAME_EXCHANGE); /* d1 (holds y) <-> p/f (a file) */
        if (r)
            err(1, "EXCHANGE directory with file");
        struct stat np, nf;
        if (stat(p, &np) || stat(d1, &nf))
            err(1, "stat after dir/file exchange");
        if (!S_ISREG(nf.st_mode))
            errx(1, "after dir/file EXCHANGE d1 is not a file");
        read_file(d1, buf, sizeof(buf));
        if (strcmp(buf, "FFF\n") != 0)
            errx(1, "after dir/file EXCHANGE d1 has the wrong content");
        snprintf(tmp, sizeof(tmp), "%s/y", pf);
        read_file(tmp, buf, sizeof(buf));
        if (strcmp(buf, "YYY\n") != 0)
            errx(1, "after dir/file EXCHANGE p/f/y has the wrong content");
        /* Linux adds one link to `p` here; the LibOS reports a constant 2 for every directory
         * (fs/libos_fs_util.c), so only "not fewer" can be checked portably */
        if (np.st_nlink < sp.st_nlink)
            errx(1, "dir/file EXCHANGE: parent nlink dropped from %lu to %lu",
                 (unsigned long)sp.st_nlink, (unsigned long)np.st_nlink);

        char saved_cwd[600];
        if (!getcwd(saved_cwd, sizeof(saved_cwd)))
            err(1, "getcwd");
        if (chdir(d2))
            err(1, "chdir d2");
        r = do_renameat2(d2, pf, RENAME_EXCHANGE); /* cwd (holds x) moves to p/f */
        if (r)
            err(1, "EXCHANGE of the cwd");
        if (access("x", F_OK))
            errx(1, "cwd lost its contents after EXCHANGE");
        char cwd_now[700];
        if (!getcwd(cwd_now, sizeof(cwd_now)))
            err(1, "getcwd after exchange");
        if (strcmp(cwd_now, pf) != 0)
            errx(1, "cwd after EXCHANGE is %s, want %s", cwd_now, pf);
        if (chdir(saved_cwd))
            err(1, "chdir back");

        snprintf(tmp, sizeof(tmp), "%s/sub", d2); /* d2 now holds y */
        if (mkdir(tmp, 0755))
            err(1, "mkdir sub");
        r = do_renameat2(d2, tmp, RENAME_EXCHANGE);
        if (r == 0 || errno != EINVAL)
            errx(1, "EXCHANGE with a descendant: want EINVAL, got r=%ld errno=%d", r, errno);
        if (rmdir(tmp))
            err(1, "rmdir sub");

        /* clean up: d2 holds y, p/f holds x, d1 is the file */
        snprintf(tmp, sizeof(tmp), "%s/y", d2);
        unlink(tmp);
        snprintf(tmp, sizeof(tmp), "%s/x", pf);
        unlink(tmp);
        if (rmdir(d2) || rmdir(pf) || rmdir(p) || unlink(d1))
            err(1, "cleanup of the directory exchange");
    } else {
        /* 6. the host-backed filesystem exchanges atomically on the host (PalFileExchange) */
        write_file(a, "AAA\n");
        write_file(b, "BBB\n");
        r = do_renameat2(a, b, RENAME_EXCHANGE);
        if (r)
            err(1, "EXCHANGE on chroot");
        read_file(a, buf, sizeof(buf));
        if (strcmp(buf, "BBB\n") != 0)
            errx(1, "chroot EXCHANGE: a does not have the old content of b");
        read_file(b, buf, sizeof(buf));
        if (strcmp(buf, "AAA\n") != 0)
            errx(1, "chroot EXCHANGE: b does not have the old content of a");
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
