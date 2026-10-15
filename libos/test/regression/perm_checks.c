/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Discretionary permission checks of path operations for an unprivileged caller (LTP stat03,
 * lstat02, readlink03, fchdir03, chroot04, rmdir03, open02): walking through a directory needs
 * search permission (EACCES), chdir/fchdir/chroot need it on the target, removing an entry needs
 * write and search permission on its directory and, in a sticky directory, ownership (EPERM),
 * O_NOATIME is for the owner, and chroot without CAP_SYS_CHROOT is EPERM. access() checks with the
 * real ids (here: root) unless AT_EACCESS (LTP access01), and a path resolution follows at most 40
 * symbolic links (ELOOP; stat03, lstat02, readlink03).
 *
 * Usage: perm_checks <scratch directory>      (runs as root, drops to uid/gid 65534)
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

#define NOBODY 65534

static char g_base[400];

static const char* path(const char* name) {
    static char buf[6][512];
    static int i;
    i = (i + 1) % 6;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", g_base, name);
    return buf[i];
}

static void touch(const char* p, mode_t mode) {
    int fd = open(p, O_CREAT | O_WRONLY, mode);
    CHECK(fd >= 0);
    close(fd);
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    umask(0);
    snprintf(g_base, sizeof(g_base), "%s/perm_checks", argv[1]);
    mkdir(g_base, 0777);
    CHECK(chmod(g_base, 0777) == 0);

    /* "closed": no permissions for others, holding a file and a symlink */
    CHECK(mkdir(path("closed"), 0700) == 0);
    touch(path("closed/file"), 0644);
    CHECK(symlink("file", path("closed/link")) == 0);
    /* "sticky": a world-writable sticky directory with root's file and directory */
    CHECK(mkdir(path("sticky"), 01777) == 0);
    touch(path("sticky/rootfile"), 0666);
    CHECK(mkdir(path("sticky/rootdir"), 0777) == 0);
    /* "ro": readable and searchable, not writable */
    CHECK(mkdir(path("ro"), 0755) == 0);
    touch(path("ro/file"), 0666);
    touch(path("rootfile"), 0644);
    int closed_fd = open(path("closed"), O_RDONLY | O_DIRECTORY);
    CHECK(closed_fd >= 0);

    CHECK(setegid(NOBODY) == 0 && seteuid(NOBODY) == 0);
    struct stat st;
    char buf[64];
    CHECK(stat(path("closed/file"), &st) == -1 && errno == EACCES);
    CHECK(lstat(path("closed/file"), &st) == -1 && errno == EACCES);
    CHECK(readlink(path("closed/link"), buf, sizeof(buf)) == -1 && errno == EACCES);
    CHECK(open(path("closed/file"), O_RDONLY) == -1 && errno == EACCES);
    CHECK(stat(path("closed"), &st) == 0); /* the directory itself is visible */
    CHECK(chdir(path("closed")) == -1 && errno == EACCES);
    CHECK(fchdir(closed_fd) == -1 && errno == EACCES);
    CHECK(chroot(path("closed")) == -1 && errno == EACCES);
    CHECK(chroot(path("ro")) == -1 && errno == EPERM); /* searchable, but no CAP_SYS_CHROOT */

    CHECK(unlink(path("ro/file")) == -1 && errno == EACCES); /* no write permission */
    CHECK(unlink(path("sticky/rootfile")) == -1 && errno == EPERM);
    CHECK(rmdir(path("sticky/rootdir")) == -1 && errno == EPERM);
    touch(path("sticky/mine"), 0644);
    CHECK(unlink(path("sticky/mine")) == 0); /* own entries may go */

    CHECK(open(path("rootfile"), O_RDONLY | O_NOATIME) == -1 && errno == EPERM);
    /* access(): the real uid is still root */
    CHECK(access(path("closed/file"), R_OK) == 0);
    CHECK(faccessat(AT_FDCWD, path("closed/file"), R_OK, AT_EACCESS) == -1 && errno == EACCES);
    CHECK(access(path("rootfile"), 8) == -1 && errno == EINVAL);
    int fd = open(path("rootfile"), O_RDONLY);
    CHECK(fd >= 0);
    close(fd);
    CHECK(seteuid(0) == 0 && setegid(0) == 0);
    close(closed_fd);

    /* 41 symbolic links in one resolution: ELOOP (each link alone is fine) */
    CHECK(mkdir(path("loop"), 0755) == 0);
    CHECK(symlink("../loop", path("loop/loop")) == 0);
    char deep[2048];
    snprintf(deep, sizeof(deep), "%s", path("loop"));
    for (int i = 0; i < 39; i++)
        strcat(deep, "/loop");
    struct stat st2;
    CHECK(stat(deep, &st2) == 0); /* 39 links */
    strcat(deep, "/loop/loop");
    CHECK(stat(deep, &st2) == -1 && errno == ELOOP); /* 41 */
    unlink(path("loop/loop"));
    rmdir(path("loop"));

    unlink(path("closed/link"));
    unlink(path("closed/file"));
    rmdir(path("closed"));
    unlink(path("sticky/rootfile"));
    rmdir(path("sticky/rootdir"));
    rmdir(path("sticky"));
    unlink(path("ro/file"));
    rmdir(path("ro"));
    unlink(path("rootfile"));
    rmdir(g_base);
    puts("TEST OK");
    return 0;
}
