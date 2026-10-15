/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Extended attributes (LTP getxattr01/03, removexattr01/02, fgetxattr03, setxattr*, listxattr*):
 * set/get/list/remove in the user namespace with the size query, ERANGE, XATTR_CREATE/REPLACE,
 * ENODATA, the descriptor variants, the namespace rules (unknown prefix EOPNOTSUPP, user.* not on
 * symlinks, EACCES without write permission) and the argument checks. On tmpfs the LibOS stores
 * the attributes itself; on a host-backed directory the host's filesystem does, and if it has no
 * user attributes the test checks that the answer is EOPNOTSUPP and stops there.
 *
 * Usage: xattr_misc <scratch directory>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    char f[512], l[512];
    snprintf(f, sizeof(f), "%s/xattr_misc_file", argv[1]);
    snprintf(l, sizeof(l), "%s/xattr_misc_link", argv[1]);
    unlink(f);
    unlink(l);
    int fd = open(f, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0);

    /* argument checks come first */
    CHECK(setxattr(f, "user.a", "1", 1, 0x10) == -1 && errno == EINVAL);
    CHECK(setxattr(f, "", "1", 1, 0) == -1 && errno == ERANGE);
    CHECK(setxattr(f, "foo.bar", "1", 1, 0) == -1 && errno == EOPNOTSUPP);

    if (setxattr(f, "user.a", "value1", 6, 0) == -1) {
        CHECK(errno == EOPNOTSUPP); /* a host filesystem without user attributes */
        puts("no user xattr support on this filesystem");
        close(fd);
        unlink(f);
        puts("TEST OK");
        return 0;
    }

    char buf[64];
    CHECK(getxattr(f, "user.a", NULL, 0) == 6); /* size query */
    CHECK(getxattr(f, "user.a", buf, sizeof(buf)) == 6 && memcmp(buf, "value1", 6) == 0);
    CHECK(getxattr(f, "user.a", buf, 2) == -1 && errno == ERANGE);
    CHECK(setxattr(f, "user.a", "x", 1, XATTR_CREATE) == -1 && errno == EEXIST);
    CHECK(setxattr(f, "user.b", "x", 1, XATTR_REPLACE) == -1 && errno == ENODATA);
    CHECK(setxattr(f, "user.a", "v2", 2, XATTR_REPLACE) == 0);
    CHECK(fgetxattr(fd, "user.a", buf, sizeof(buf)) == 2 && memcmp(buf, "v2", 2) == 0);
    CHECK(fsetxattr(fd, "user.b", "", 0, XATTR_CREATE) == 0); /* empty values exist */
    CHECK(getxattr(f, "user.b", buf, sizeof(buf)) == 0);

    char list[256];
    ssize_t n = listxattr(f, list, sizeof(list));
    CHECK(n > 0);
    int seen_a = 0, seen_b = 0;
    for (char* p = list; p < list + n; p += strlen(p) + 1) {
        seen_a |= !strcmp(p, "user.a");
        seen_b |= !strcmp(p, "user.b");
    }
    CHECK(seen_a && seen_b);
    CHECK(listxattr(f, NULL, 0) == n);
    CHECK(flistxattr(fd, list, 1) == -1 && errno == ERANGE);

    CHECK(removexattr(f, "user.a") == 0);
    CHECK(getxattr(f, "user.a", buf, sizeof(buf)) == -1 && errno == ENODATA);
    CHECK(removexattr(f, "user.a") == -1 && errno == ENODATA);
    CHECK(fremovexattr(fd, "user.b") == 0);

    /* user attributes are not allowed on symbolic links */
    CHECK(symlink("xattr_misc_file", l) == 0);
    CHECK(lsetxattr(l, "user.a", "1", 1, 0) == -1 && errno == EPERM);
    CHECK(setxattr(l, "user.c", "1", 1, 0) == 0); /* follows the link to the file */
    CHECK(getxattr(f, "user.c", buf, sizeof(buf)) == 1);

    /* writing needs write permission on the file */
    CHECK(fchmod(fd, 0444) == 0);
    CHECK(seteuid(65534) == 0);
    CHECK(setxattr(f, "user.d", "1", 1, 0) == -1 && errno == EACCES);
    CHECK(getxattr(f, "user.c", buf, sizeof(buf)) == 1); /* reading is allowed */
    CHECK(seteuid(0) == 0);

    close(fd);
    unlink(l);
    unlink(f);
    puts("TEST OK");
    return 0;
}
