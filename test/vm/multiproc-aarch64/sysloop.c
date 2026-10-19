/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * sysloop -- time of a syscall-heavy loop: 2,000,000 getpid() calls (the cheapest LibOS call:
 * the cost is the EL0 -> EL1 entry, the register snapshot and the return) and 100,000
 * write+read pairs on a pipe. Prints nanoseconds per call.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(void) {
    enum { N = 2000000, M = 100000 };
    double t0 = now();
    for (int i = 0; i < N; i++)
        syscall(SYS_getpid);
    double t1 = now();
    int fds[2];
    char c = 'x';
    if (pipe(fds) < 0)
        return 2;
    for (int i = 0; i < M; i++) {
        if (write(fds[1], &c, 1) != 1 || read(fds[0], &c, 1) != 1)
            return 3;
    }
    double t2 = now();
    printf("sysloop: getpid %.1f ns/call, pipe write+read %.1f ns/pair\n", (t1 - t0) / N,
           (t2 - t1) / M);
    return 0;
}
