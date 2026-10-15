/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * O_APPEND and RLIMIT_FSIZE (LTP pwrite04, llseek01, truncate03): with O_APPEND every write()
 * and writev() starts at the end of the file, and so does pwrite() (Linux ignores its offset
 * then); beyond RLIMIT_FSIZE a write and a growing truncate are EFBIG with SIGXFSZ, and a write
 * across the limit is shortened.
 *
 * Usage: append_fsize <scratch directory>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

static off_t size_of(int fd) {
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    return st.st_size;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    char path[512];
    snprintf(path, sizeof(path), "%s/append_fsize_file", argv[1]);
    unlink(path);

    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "aaaa", 4) == 4);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, "bbbb", 4) == 4); /* at the end, not at 0 */
    CHECK(size_of(fd) == 8);
    CHECK(pwrite(fd, "cccc", 4, 0) == 4); /* pwrite on O_APPEND appends too */
    CHECK(size_of(fd) == 12);
    struct iovec iov[2] = {{"dd", 2}, {"ee", 2}};
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(writev(fd, iov, 2) == 4);
    CHECK(size_of(fd) == 16);
    char buf[17] = {0};
    CHECK(pread(fd, buf, 16, 0) == 16 && strcmp(buf, "aaaabbbbccccddee") == 0);
    close(fd);

    /* RLIMIT_FSIZE */
    CHECK(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
    struct rlimit rl = {.rlim_cur = 4096, .rlim_max = RLIM_INFINITY};
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
    fd = open(path, O_RDWR | O_TRUNC);
    CHECK(fd >= 0);
    char big[100];
    memset(big, 'x', sizeof(big));
    CHECK(lseek(fd, 4090, SEEK_SET) == 4090);
    CHECK(write(fd, big, sizeof(big)) == 6); /* shortened at the limit */
    CHECK(write(fd, big, sizeof(big)) == -1 && errno == EFBIG);
    CHECK(pwrite(fd, big, 10, 5000) == -1 && errno == EFBIG);
    CHECK(ftruncate(fd, 8192) == -1 && errno == EFBIG);
    CHECK(truncate(path, 8192) == -1 && errno == EFBIG);
    CHECK(ftruncate(fd, 4096) == 0 && ftruncate(fd, 100) == 0); /* up to the limit is fine */
    close(fd);
    rl.rlim_cur = RLIM_INFINITY;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);

    unlink(path);
    puts("TEST OK");
    return 0;
}
