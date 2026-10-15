/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Ownership and set-ID rules of the kernel (LTP chmod05, fchmod05, chown02/03, fchown02/03,
 * fchownat01/02, mknod02-05, creat08, open10): objects created in a set-group-ID directory take
 * its group (directories also the S_ISGID bit), chown clears S_ISUID and (with group-execute)
 * S_ISGID of a non-directory, chmod drops S_ISGID for a caller outside the file's group and is
 * refused to a non-owner, chown is refused without CAP_CHOWN, fchownat validates its flags and
 * honours AT_SYMLINK_NOFOLLOW.
 *
 * Usage: setid_rules <scratch directory>        (runs as root, drops to uid/gid 65534 for parts)
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

#define GROUP 4242
#define NOBODY 65534

static char g_base[400];

static const char* path(const char* name) {
    static char buf[4][512];
    static int i;
    i = (i + 1) % 4;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", g_base, name);
    return buf[i];
}

static mode_t mode_of(const char* p) {
    struct stat st;
    CHECK(lstat(p, &st) == 0);
    return st.st_mode;
}

static gid_t gid_of(const char* p) {
    struct stat st;
    CHECK(lstat(p, &st) == 0);
    return st.st_gid;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    snprintf(g_base, sizeof(g_base), "%s/setid_rules", argv[1]);
    mkdir(g_base, 0777);
    CHECK(chmod(g_base, 0777) == 0);
    umask(0);

    /* a set-group-ID directory owned by GROUP */
    const char* sg = path("sgdir");
    CHECK(mkdir(sg, 0777) == 0);
    CHECK(chown(sg, 0, GROUP) == 0);
    CHECK(chmod(sg, 02777) == 0);
    CHECK((mode_of(sg) & 07777) == 02777);

    char p[600];
    snprintf(p, sizeof(p), "%s/file", sg);
    int fd = open(p, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0);
    close(fd);
    CHECK(gid_of(p) == GROUP); /* inherited group */
    snprintf(p, sizeof(p), "%s/sub", sg);
    CHECK(mkdir(p, 0755) == 0);
    CHECK(gid_of(p) == GROUP && (mode_of(p) & S_ISGID)); /* and the bit, for a directory */
    snprintf(p, sizeof(p), "%s/fifo", sg);
    CHECK(mknod(p, S_IFIFO | 0644, 0) == 0);
    CHECK(gid_of(p) == GROUP);

    /* chown of a non-directory clears S_ISUID, and S_ISGID when group-execute is set */
    const char* f = path("suidfile");
    fd = open(f, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0);
    CHECK(fchmod(fd, 06770) == 0);
    CHECK((mode_of(f) & 07777) == 06770);
    CHECK(fchown(fd, 0, 0) == 0);
    CHECK((mode_of(f) & 07777) == 0770);
    CHECK(fchmod(fd, 02750 & ~S_IXGRP) == 0); /* S_ISGID without group-execute survives chown */
    CHECK(chown(f, 0, 0) == 0);
    CHECK((mode_of(f) & 07777) == (02750 & ~S_IXGRP));
    close(fd);
    CHECK(chown(sg, 0, GROUP) == 0 && (mode_of(sg) & 07777) == 02777); /* directories keep it */

    /* fchownat flags */
    CHECK(fchownat(AT_FDCWD, f, 0, 0, 0x4000) == -1 && errno == EINVAL);
    const char* l = path("link");
    unlink(l);
    CHECK(symlink(f, l) == 0);
    CHECK(fchownat(AT_FDCWD, l, 0, GROUP, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(gid_of(l) == GROUP && gid_of(f) == 0); /* the link changed, not its target */

    /* as an unprivileged user outside GROUP */
    CHECK(setegid(NOBODY) == 0 && seteuid(NOBODY) == 0);
    snprintf(p, sizeof(p), "%s/mine", sg);
    fd = open(p, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0);
    CHECK(gid_of(p) == GROUP); /* inherited, though not a member */
    CHECK(fchmod(fd, 02755) == 0);
    CHECK((mode_of(p) & 07777) == 0755); /* not in the group: S_ISGID dropped */
    CHECK(fchown(fd, 0, -1) == -1 && errno == EPERM); /* giving the file away */
    CHECK(fchown(fd, -1, GROUP + 1) == -1 && errno == EPERM); /* a group it is not in */
    CHECK(fchown(fd, NOBODY, NOBODY) == 0); /* its own uid and group */
    close(fd);
    CHECK(chmod(f, 0777) == -1 && errno == EPERM); /* not the owner */
    CHECK(chown(f, NOBODY, -1) == -1 && errno == EPERM);
    CHECK(seteuid(0) == 0 && setegid(0) == 0);

    /* clean up */
    unlink(l);
    unlink(f);
    snprintf(p, sizeof(p), "%s/mine", sg);
    unlink(p);
    snprintf(p, sizeof(p), "%s/file", sg);
    unlink(p);
    snprintf(p, sizeof(p), "%s/fifo", sg);
    unlink(p);
    snprintf(p, sizeof(p), "%s/sub", sg);
    rmdir(p);
    rmdir(sg);
    rmdir(g_base);
    puts("TEST OK");
    return 0;
}
