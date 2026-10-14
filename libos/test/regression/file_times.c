/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * Regression tests for the file-timestamp syscalls: utimensat, utimes, utime, futimesat.
 *
 * The LTP tests for these calls break in their own setup in this container (they need a test
 * device or a tmpfs mount), so these cases are checked here instead: explicit values, subsecond
 * values, UTIME_NOW / UTIME_OMIT, directory-fd (`AT_EMPTY_PATH`) and relative-path operation,
 * the `futimesat` dirfd semantics (NULL pathname means `futimes`), and the argument-validation
 * results (EFAULT / EBADF / EINVAL / ENOTDIR / ENOENT).
 *
 * Usage: file_times <base-dir>   (files are created as <base-dir>/fts_*)
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
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#ifdef __x86_64__
#define HAVE_FUTIMESAT 1
#define SYS_NR_futimesat 261
#endif

/* The UABI values (linux/utime.h is not included to avoid clashing with "stat.h"). */
#ifndef UTIME_OMIT
#define UTIME_OMIT ((1L << 30) - 2)
#endif
#ifndef UTIME_NOW
#define UTIME_NOW ((1L << 30) - 1)
#endif

static char base[512];

static void path_of(char* buf, size_t sz, const char* name) {
    if ((size_t)snprintf(buf, sz, "%s/%s", base, name) >= sz)
        abort();
}

static void touch_file(const char* path) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        err(1, "open %s", path);
    char c = 'x';
    if (write(fd, &c, 1) != 1)
        err(1, "write %s", path);
    close(fd);
}

static void get_times(const char* path, struct stat* st) {
    if (stat(path, st) < 0)
        err(1, "stat %s", path);
}

static void expect_times(const char* what, const struct stat* st,
                         time_t atime, long atime_nsec, time_t mtime, long mtime_nsec) {
    if (st->st_atime != atime || st->st_atim.tv_nsec != atime_nsec ||
            st->st_mtime != mtime || st->st_mtim.tv_nsec != mtime_nsec) {
        errx(1, "%s: got atime %ld.%09ld mtime %ld.%09ld,"
               " expected %ld.%09ld %ld.%09ld", what,
             (long)st->st_atime, st->st_atim.tv_nsec, (long)st->st_mtime, st->st_mtim.tv_nsec,
             (long)atime, atime_nsec, (long)mtime, mtime_nsec);
    }
}

#ifdef HAVE_FUTIMESAT
static long syscall_futimesat(int dirfd, const char* path, const struct timeval times[2]) {
    return syscall(SYS_NR_futimesat, dirfd, path, times);
}
#endif

int main(int argc, const char** argv) {
    if (argc != 2)
        errx(1, "usage: file_times <base-dir>");
    snprintf(base, sizeof(base), "%s", argv[1]);

    char file[600], dir[600], relfile[600], link[600], target[600];
    path_of(file, sizeof(file), "fts_file");
    path_of(dir, sizeof(dir), "fts_dir");
    path_of(relfile, sizeof(relfile), "fts_dir/fts_rel");
    path_of(link, sizeof(link), "fts_link");
    path_of(target, sizeof(target), "fts_target");

    /* clean leftovers from a previous run */
    unlink(file);
    unlink(relfile);
    unlink(link);
    unlink(target);
    rmdir(dir);

    touch_file(file);

    struct timespec ts[2];
    struct stat st;

    /* 1. utimensat: explicit seconds */
    ts[0] = (struct timespec){.tv_sec = 1000, .tv_nsec = 0};
    ts[1] = (struct timespec){.tv_sec = 2000, .tv_nsec = 0};
    if (utimensat(AT_FDCWD, file, ts, 0))
        err(1, "utimensat explicit");
    get_times(file, &st);
    expect_times("explicit seconds", &st, 1000, 0, 2000, 0);

    /* 2. utimensat: explicit nanoseconds must round-trip through stat (subsecond support) */
    ts[0] = (struct timespec){.tv_sec = 555, .tv_nsec = 123456789};
    ts[1] = (struct timespec){.tv_sec = 666, .tv_nsec = 987654321};
    if (utimensat(AT_FDCWD, file, ts, 0))
        err(1, "utimensat nsec");
    get_times(file, &st);
    expect_times("explicit nanoseconds", &st, 555, 123456789, 666, 987654321);

    /* 3. UTIME_OMIT leaves the corresponding timestamp alone */
    ts[0] = (struct timespec){.tv_sec = 777, .tv_nsec = 0};
    ts[1] = (struct timespec){.tv_nsec = UTIME_OMIT};
    if (utimensat(AT_FDCWD, file, ts, 0))
        err(1, "utimensat UTIME_OMIT");
    get_times(file, &st);
    expect_times("UTIME_OMIT mtime", &st, 777, 0, 666, 987654321);

    /* 4. UTIME_NOW picks the current time (within the [before, after] window) */
    struct timeval before;
    gettimeofday(&before, NULL);
    ts[0] = (struct timespec){.tv_nsec = UTIME_NOW};
    ts[1] = (struct timespec){.tv_nsec = UTIME_NOW};
    if (utimensat(AT_FDCWD, file, ts, 0))
        err(1, "utimensat UTIME_NOW");
    struct timeval after;
    gettimeofday(&after, NULL);
    get_times(file, &st);
    if (st.st_mtime < before.tv_sec || st.st_mtime > after.tv_sec + 1)
        errx(1, "UTIME_NOW: mtime %ld not in [%ld, %ld]", (long)st.st_mtime,
             (long)before.tv_sec, (long)after.tv_sec);

    /* 5. NULL times means "now" as well */
    if (utimensat(AT_FDCWD, file, NULL, 0))
        err(1, "utimensat NULL times");
    get_times(file, &st);
    if (st.st_mtime < before.tv_sec)
        errx(1, "utimensat NULL times did not update mtime");

    /* 6. utimensat(fd, "", AT_EMPTY_PATH) operates on the opened file */
    int fd = open(file, O_RDONLY);
    if (fd < 0)
        err(1, "open for AT_EMPTY_PATH");
    ts[0] = (struct timespec){.tv_sec = 3000, .tv_nsec = 0};
    ts[1] = (struct timespec){.tv_sec = 4000, .tv_nsec = 0};
    if (utimensat(fd, "", ts, AT_EMPTY_PATH))
        err(1, "utimensat AT_EMPTY_PATH");
    close(fd);
    get_times(file, &st);
    expect_times("AT_EMPTY_PATH", &st, 3000, 0, 4000, 0);

    /* 7. a directory through its directory fd */
    if (mkdir(dir, 0755) && errno != EEXIST)
        err(1, "mkdir %s", dir);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0)
        err(1, "open dir");
    ts[0] = (struct timespec){.tv_sec = 5000, .tv_nsec = 0};
    ts[1] = (struct timespec){.tv_sec = 6000, .tv_nsec = 0};
    if (utimensat(dfd, "", ts, AT_EMPTY_PATH))
        err(1, "utimensat on directory fd");

#ifdef HAVE_FUTIMESAT
    /* 8. futimesat with a path relative to the directory fd (the file lives inside `dir`) */
    unlink(relfile);
    touch_file(relfile);
    const char* relname = "fts_rel";
    struct timeval ftv[2] = {{.tv_sec = 1111, .tv_usec = 1}, {.tv_sec = 2222, .tv_usec = 2}};
    if (syscall_futimesat(dfd, relname, ftv) < 0)
        err(1, "futimesat relative");
    get_times(relfile, &st);
    expect_times("futimesat relative", &st, 1111, 1000, 2222, 2000);

    /* 9. futimesat(dirfd, NULL) is the old futimes(): apply to the directory from step 7 */
    ftv[0] = (struct timeval){.tv_sec = 7000, .tv_usec = 0};
    ftv[1] = (struct timeval){.tv_sec = 8000, .tv_usec = 0};
    if (syscall_futimesat(dfd, NULL, ftv) < 0)
        err(1, "futimesat NULL pathname");
    struct stat dst;
    if (stat(dir, &dst) < 0)
        err(1, "stat dir");
    expect_times("futimesat(dirfd, NULL)", &dst, 7000, 0, 8000, 0);

    /* 10. futimesat error results */
    long r = syscall_futimesat(400, NULL, ftv);
    if (r != -1 || errno != EBADF)
        errx(1, "futimesat(bad fd, NULL): got %ld/%d, want -1/EBADF", r, errno);
    int ffd = open(file, O_RDONLY);
    r = syscall_futimesat(ffd, "fts_rel", ftv);
    if (r != -1 || errno != ENOTDIR)
        errx(1, "futimesat(file fd, relative): got %ld/%d, want -1/ENOTDIR", r, errno);
    close(ffd);
    r = syscall_futimesat(dfd, (const char*)(long)-1, ftv);
    if (r != -1 || errno != EFAULT)
        errx(1, "futimesat(bad pathname): got %ld/%d, want -1/EFAULT", r, errno);
    r = syscall_futimesat(dfd, relname, (const struct timeval*)(long)-1);
    if (r != -1 || errno != EFAULT)
        errx(1, "futimesat(bad times): got %ld/%d, want -1/EFAULT", r, errno);
#endif /* HAVE_FUTIMESAT */

    /* 11. utime() with struct utimbuf */
    touch_file(target);
    struct utimbuf ub = {.actime = 7, .modtime = 9};
    if (utime(target, &ub))
        err(1, "utime");
    get_times(target, &st);
    expect_times("utime", &st, 7, 0, 9, 0);

    /* 12. utimes() with timeval (microseconds arrive as nanoseconds) */
    struct timeval tv[2] = {{.tv_sec = 3, .tv_usec = 500000}, {.tv_sec = 4, .tv_usec = 250000}};
    if (utimes(target, tv))
        err(1, "utimes");
    get_times(target, &st);
    expect_times("utimes", &st, 3, 500000000, 4, 250000000);

    /* 13. argument validation */
    struct timeval badtv[2] = {{.tv_sec = 1, .tv_usec = 1000000}, {.tv_sec = 0, .tv_usec = 0}};
    if (utimes(target, badtv) == 0 || errno != EINVAL)
        errx(1, "utimes(tv_usec=1000000): want EINVAL, got %d", errno);
    if (utimensat(AT_FDCWD, (const char*)(long)-1, NULL, 0) == 0 || errno != EFAULT)
        errx(1, "utimensat(bad path): want EFAULT, got %d", errno);
    if (utimensat(AT_FDCWD, file, (const struct timespec*)(long)-1, 0) == 0 ||
            errno != EFAULT) {
        errx(1, "utimensat(bad times): want EFAULT, got %d", errno);
    }
    if (utimensat(AT_FDCWD, file, NULL, 0x10000) == 0 || errno != EINVAL)
        errx(1, "utimensat(bad flags): want EINVAL, got %d", errno);
    ts[0] = (struct timespec){.tv_sec = 1, .tv_nsec = 1000000000};
    ts[1] = (struct timespec){.tv_sec = 1, .tv_nsec = 0};
    if (utimensat(AT_FDCWD, file, ts, 0) == 0 || errno != EINVAL)
        errx(1, "utimensat(tv_nsec=1e9): want EINVAL, got %d", errno);
    if (utimensat(AT_FDCWD, "fts_does_not_exist", NULL, 0) == 0 || errno != ENOENT)
        errx(1, "utimensat(missing): want ENOENT, got %d", errno);
    if (utime("fts_does_not_exist", NULL) == 0 || errno != ENOENT)
        errx(1, "utime(missing): want ENOENT, got %d", errno);

    /* 14. AT_SYMLINK_NOFOLLOW is accepted and reaches the link's dentry (checked via lstat
     * only for the return code here: the host-backed filesystem sets times through the file
     * handle and may follow the link; the tmpfs variant below checks the values). */
    unlink(link);
    if (symlink("fts_target", link))
        err(1, "symlink");
    ts[0] = (struct timespec){.tv_sec = 12345, .tv_nsec = 0};
    ts[1] = (struct timespec){.tv_sec = 12346, .tv_nsec = 0};
    if (utimensat(AT_FDCWD, link, ts, AT_SYMLINK_NOFOLLOW))
        err(1, "utimensat AT_SYMLINK_NOFOLLOW");
    struct stat lst;
    if (lstat(link, &lst) < 0)
        err(1, "lstat link");
    if (S_ISLNK(lst.st_mode)) {
        /* tmpfs stores link times exactly; check only there (base under /mnt/tmpfs). */
        if (strncmp(base, "/mnt/tmpfs", 10) == 0)
            expect_times("link times (tmpfs)", &lst, 12345, 0, 12346, 0);
    } else {
        errx(1, "lstat link is not a symlink");
    }

    close(dfd);
    unlink(file);
    unlink(relfile);
    unlink(link);
    unlink(target);
    rmdir(dir);

    puts("TEST OK");
    return 0;
}
