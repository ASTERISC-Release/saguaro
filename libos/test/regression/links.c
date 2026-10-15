/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * Regression tests for symlinks and hard links: creation and following, dangling targets,
 * ELOOP, EEXIST/ENOENT/ENOTDIR/EPERM/EINVAL results, and hard-link counts and shared content.
 *
 * The LTP symlink/link groups cover the basics but several cases (dangling links through
 * rename, loops, link counts on unlink) are checked here because the device-based LTP tests
 * cannot run in this container.
 *
 * Usage: links <base-dir>   (files are created as <base-dir>/lnk_*)
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

static char base[512];

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

static long sys_linkat(int oldfd, const char* old, int newfd, const char* new, int flags) {
    return syscall(SYS_linkat, oldfd, old, newfd, new, flags);
}

static long sys_symlinkat(const char* target, int newfd, const char* new) {
    return syscall(SYS_symlinkat, target, newfd, new);
}

int main(int argc, const char** argv) {
    if (argc != 2)
        errx(1, "usage: links <base-dir>");
    snprintf(base, sizeof(base), "%s", argv[1]);

    char a[600], b[600], l1[600], l2[600], dangling[600], d[600], inner[600];
    path_of(a, sizeof(a), "lnk_a");
    path_of(b, sizeof(b), "lnk_b");
    path_of(l1, sizeof(l1), "lnk_l1");
    path_of(l2, sizeof(l2), "lnk_l2");
    path_of(dangling, sizeof(dangling), "lnk_dangling");
    path_of(d, sizeof(d), "lnk_dir");
    path_of(inner, sizeof(inner), "lnk_dir/inner");

    unlink(a);
    unlink(b);
    unlink(l1);
    unlink(l2);
    unlink(dangling);
    unlink(inner);
    rmdir(d);

    struct stat st, st2;
    char buf[64];
    long r;

    /* 1. symlink basics: lstat sees the link, stat follows it, readlink returns the target.
     * The target is stored relative to the link's directory, so use the plain file name. */
    write_file(a, "alpha\n");
    const char* aname = "lnk_a";
    if (symlink(aname, l1))
        err(1, "symlink");
    if (lstat(l1, &st) < 0)
        err(1, "lstat link");
    if (!S_ISLNK(st.st_mode))
        errx(1, "lstat: not a symlink");
    if (st.st_size != (off_t)strlen(aname))
        errx(1, "link size %ld != target name length %zu", (long)st.st_size, strlen(aname));
    if (stat(l1, &st2) < 0)
        err(1, "stat through link");
    if (!S_ISREG(st2.st_mode) || st2.st_size != 6)
        errx(1, "stat through link: wrong file");
    char target_buf[600];
    ssize_t n = readlink(l1, target_buf, sizeof(target_buf));
    if (n != (ssize_t)strlen(aname) || memcmp(target_buf, aname, n) != 0)
        errx(1, "readlink returned a wrong target");

    /* 2. readlink on a non-symlink is EINVAL */
    if (readlink(a, target_buf, sizeof(target_buf)) != -1 || errno != EINVAL)
        errx(1, "readlink(non-link): want EINVAL, got %d", errno);

    /* 3. dangling symlink: open/stat fail with ENOENT, lstat works, unlink keeps the target */
    if (symlink("/no/such/target", dangling))
        err(1, "symlink dangling");
    if (stat(dangling, &st) == 0 || errno != ENOENT)
        errx(1, "stat(dangling): want ENOENT, got %d", errno);
    if (open(dangling, O_RDONLY) != -1 || errno != ENOENT)
        errx(1, "open(dangling): want ENOENT, got %d", errno);
    if (lstat(dangling, &st) < 0)
        err(1, "lstat(dangling)");

    /* 4. symlink() onto an existing name is EEXIST (file and directory) */
    if (symlink("/tmp", a) == 0 || errno != EEXIST)
        errx(1, "symlink(existing file): want EEXIST, got %d", errno);
    if (mkdir(d, 0755) && errno != EEXIST)
        err(1, "mkdir");
    if (symlink("/tmp", d) == 0 || errno != EEXIST)
        errx(1, "symlink(existing dir): want EEXIST, got %d", errno);

    /* 5. symlinkat relative to a directory fd */
    int dfd = open(base, O_RDONLY | O_DIRECTORY);
    if (dfd < 0)
        err(1, "open base dir");
    r = sys_symlinkat("lnk_relative_target", dfd, "lnk_rel");
    if (r)
        err(1, "symlinkat");
    char rel[600];
    path_of(rel, sizeof(rel), "lnk_rel");
    if (lstat(rel, &st) < 0 || !S_ISLNK(st.st_mode))
        errx(1, "symlinkat created nothing");
    unlink(rel);

    /* 6. symlink loop: ELOOP */
    unlink(l1);
    unlink(l2);
    if (symlink("lnk_l2", l1) || symlink("lnk_l1", l2))
        err(1, "loop symlinks");
    if (stat(l1, &st) == 0 || errno != ELOOP)
        errx(1, "stat(loop): want ELOOP, got %d", errno);
    unlink(l1);
    unlink(l2);

    /* 7. hard link: shared inode and content, nlink accounting */
    if (link(a, b))
        err(1, "link");
    stat(a, &st);
    stat(b, &st2);
    if (st.st_ino != st2.st_ino || st.st_nlink != 2 || st2.st_nlink != 2)
        errx(1, "after link: ino %lu/%lu nlink %lu/%lu", (unsigned long)st.st_ino,
             (unsigned long)st2.st_ino, (unsigned long)st.st_nlink, (unsigned long)st2.st_nlink);
    write_file(b, "beta!\n");
    read_file(a, buf, sizeof(buf));
    if (strcmp(buf, "beta!\n") != 0)
        errx(1, "hard link does not share content: %s", buf);

    /* 7b. rename() between two links of one file does nothing and succeeds (LTP rename13) */
    if (rename(a, b))
        err(1, "rename(link, link)");
    if (stat(a, &st) || stat(b, &st2) || st.st_ino != st2.st_ino)
        errx(1, "rename(link, link) removed a name");

    /* 8. link() onto an existing name is EEXIST; a missing source is ENOENT */
    if (link(a, a) == 0 || errno != EEXIST)
        errx(1, "link(existing): want EEXIST, got %d", errno);
    if (link("/no/such/file", b) == 0 || errno != ENOENT)
        errx(1, "link(missing source): want ENOENT, got %d", errno);

    /* 9. link() on a symlink makes a hard link to the link itself (Linux semantics, the source
     * is not dereferenced unless AT_SYMLINK_FOLLOW); this works even for a dangling target */
    if (symlink("/no/such/target", l1))
        err(1, "symlink dangling 2");
    if (link(l1, l2))
        err(1, "link(symlink) should create a hard link to the link");
    if (lstat(l2, &st) < 0 || !S_ISLNK(st.st_mode))
        errx(1, "the hard link is not a symlink");
    if (st.st_nlink != 2)
        errx(1, "hard link to symlink: nlink %lu, want 2", (unsigned long)st.st_nlink);
    n = readlink(l2, target_buf, sizeof(target_buf));
    if (n != (ssize_t)strlen("/no/such/target") ||
            memcmp(target_buf, "/no/such/target", n) != 0) {
        errx(1, "readlink on the hard link to the symlink failed");
    }
    unlink(l2);
    unlink(l1);

    /* 10. link() of a directory is EPERM; onto an existing name EEXIST comes first */
    if (link(d, b) == 0 || errno != EEXIST)
        errx(1, "link(directory, existing): want EEXIST, got %d", errno);
    char dlink[600];
    path_of(dlink, sizeof(dlink), "lnk_dirlink");
    unlink(dlink);
    if (link(d, dlink) == 0 || errno != EPERM)
        errx(1, "link(directory): want EPERM, got %d", errno);

    /* 11. linkat with bad flags is EINVAL */
    r = sys_linkat(AT_FDCWD, a, AT_FDCWD, b, 0x1234);
    if (r == 0 || errno != EINVAL)
        errx(1, "linkat(bad flags): want EINVAL, got %d", errno);

    /* 12. unlink drops one reference, the other still sees the data */
    if (unlink(b))
        err(1, "unlink b");
    stat(a, &st);
    if (st.st_nlink != 1)
        errx(1, "after unlink: nlink %lu, want 1", (unsigned long)st.st_nlink);
    read_file(a, buf, sizeof(buf));
    if (strcmp(buf, "beta!\n") != 0)
        errx(1, "content lost after unlink of the other link");

    /* 13. renaming over a symlink replaces the link, not the target */
    write_file(a, "gamma.\n");
    write_file(b, "delta-\n");
    if (symlink("lnk_b", l1))
        err(1, "symlink for rename");
    if (rename(a, l1))
        err(1, "rename over symlink");
    read_file(b, buf, sizeof(buf));
    if (strcmp(buf, "delta-\n") != 0)
        errx(1, "rename replaced the symlink target");
    read_file(l1, buf, sizeof(buf));
    if (strcmp(buf, "gamma.\n") != 0)
        errx(1, "rename did not replace the symlink");

    /* 14. rmdir on a non-empty directory is ENOTEMPTY, on an empty one succeeds */
    write_file(inner, "x");
    if (rmdir(d) == 0 || errno != ENOTEMPTY)
        errx(1, "rmdir(non-empty): want ENOTEMPTY, got %d", errno);
    /* the same through unlinkat(AT_REMOVEDIR), which is what rmdir() is on aarch64 */
    if (unlinkat(AT_FDCWD, d, AT_REMOVEDIR) == 0 || errno != ENOTEMPTY)
        errx(1, "unlinkat(non-empty, AT_REMOVEDIR): want ENOTEMPTY, got %d", errno);
    unlink(inner);
    if (rmdir(d))
        err(1, "rmdir(empty)");

    /* 15. symlink with an empty target name is EINVAL at resolution, creation itself works */
    close(dfd);
    /* Remove every symlink within this session: the host-backed filesystem cannot re-discover a
     * symlink created by an earlier run (its lookup follows the link), so leftovers would break
     * later runs. */
    unlink(dangling);
    unlink(a);
    unlink(b);
    unlink(l1);
    unlink(l2);

    puts("TEST OK");
    return 0;
}
