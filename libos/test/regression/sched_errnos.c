/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * The scheduler and priority calls check their target like the kernel (LTP sched_getparam03,
 * sched_setparam04, sched_getscheduler02, sched_setscheduler01, getpriority02): a NULL parameter
 * or a negative pid is EINVAL, a bad pointer EFAULT, and a pid that names no process ESRCH.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

#define NO_SUCH_PID 4000000 /* above the default pid_max */

int main(void) {
    struct sched_param p = {0};
    CHECK(sched_getparam(0, &p) == 0 && sched_getparam(getpid(), &p) == 0);
    CHECK(sched_getparam(NO_SUCH_PID, &p) == -1 && errno == ESRCH);
    CHECK(syscall(SYS_sched_getparam, -1, &p) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_sched_getparam, 0, NULL) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_sched_setparam, NO_SUCH_PID, &p) == -1 && errno == ESRCH);
    CHECK(syscall(SYS_sched_setparam, 0, NULL) == -1 && errno == EINVAL);
    CHECK(sched_getscheduler(NO_SUCH_PID) == -1 && errno == ESRCH);
    CHECK(sched_setscheduler(NO_SUCH_PID, SCHED_OTHER, &p) == -1 && errno == ESRCH);
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, (void*)-1) == -1 && errno == EFAULT);
    CHECK(getpriority(PRIO_PROCESS, 0) >= -20);
    errno = 0;
    CHECK(getpriority(PRIO_PROCESS, NO_SUCH_PID) == -1 && errno == ESRCH);
    CHECK(getpriority(77, 0) == -1 && errno == EINVAL);
    puts("TEST OK");
    return 0;
}
