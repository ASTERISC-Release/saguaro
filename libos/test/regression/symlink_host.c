/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Symbolic links created on the host before Saguaro started (the test harness makes them) are
 * seen as links: lstat() reports S_IFLNK, readlink() returns the target, and a dangling link is
 * found by lstat() although stat() fails with ENOENT.
 *
 * Usage: symlink_host <live link> <expected target> <dangling link>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 4)
        return 1;
    struct stat st;
    char buf[256];

    if (lstat(argv[1], &st) || !S_ISLNK(st.st_mode)) {
        printf("lstat of live link: not a link\n");
        return 1;
    }
    ssize_t n = readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        perror("readlink");
        return 1;
    }
    buf[n] = '\0';
    if (strcmp(buf, argv[2])) {
        printf("readlink: got '%s', want '%s'\n", buf, argv[2]);
        return 1;
    }
    if (stat(argv[1], &st) || !S_ISREG(st.st_mode)) {
        printf("stat of live link does not reach the file\n");
        return 1;
    }

    if (lstat(argv[3], &st) || !S_ISLNK(st.st_mode)) {
        printf("lstat of dangling link: not found as a link (errno=%d)\n", errno);
        return 1;
    }
    if (stat(argv[3], &st) == 0 || errno != ENOENT) {
        printf("stat of dangling link: want ENOENT\n");
        return 1;
    }
    puts("TEST OK");
    return 0;
}
