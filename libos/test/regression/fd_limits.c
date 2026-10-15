/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * RLIMIT_NOFILE is the descriptor limit (LTP open04, creat05, dup03, dup06, dup205, pipe06,
 * pipe07, getdtablesize01, setrlimit03): open(), dup(), pipe() fail with EMFILE at the limit,
 * dup2() to a number beyond it with EBADF, getdtablesize() reports it, and RLIMIT_NOFILE cannot
 * exceed /proc/sys/fs/nr_open even for root.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

#define LIMIT 64

int main(void) {
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    struct rlimit low = {.rlim_cur = LIMIT, .rlim_max = rl.rlim_max};
    CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0);
    CHECK(getdtablesize() == LIMIT);

    int last = -1, opened = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            CHECK(errno == EMFILE);
            break;
        }
        CHECK(fd < LIMIT);
        last = fd;
        opened++;
        CHECK(opened <= LIMIT);
    }
    CHECK(last == LIMIT - 1); /* every number below the limit was handed out */
    CHECK(dup(0) == -1 && errno == EMFILE);
    int p[2];
    CHECK(pipe(p) == -1 && errno == EMFILE);
    CHECK(fcntl(0, F_DUPFD, 0) == -1 && errno == EMFILE);

    close(last);
    CHECK(dup2(0, LIMIT) == -1 && errno == EBADF); /* beyond the limit */
    CHECK(dup2(0, LIMIT - 1) == LIMIT - 1);         /* the last free number */

    /* RLIMIT_NOFILE above nr_open is EPERM, also for root */
    unsigned int nr_open = 0;
    FILE* f = fopen("/proc/sys/fs/nr_open", "r");
    if (f) {
        CHECK(fscanf(f, "%u", &nr_open) == 1);
        fclose(f);
        struct rlimit too_big = {.rlim_cur = LIMIT, .rlim_max = (rlim_t)nr_open + 1};
        CHECK(setrlimit(RLIMIT_NOFILE, &too_big) == -1 && errno == EPERM);
    }
    struct rlimit inverted = {.rlim_cur = LIMIT + 1, .rlim_max = LIMIT};
    CHECK(setrlimit(RLIMIT_NOFILE, &inverted) == -1 && errno == EINVAL);

    for (int fd = 3; fd < LIMIT; fd++)
        close(fd);
    puts("TEST OK");
    return 0;
}
