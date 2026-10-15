/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * prctl() options and their argument checks (LTP prctl01, prctl02, prctl05): the parent-death
 * signal, the thread name, dumpable, no_new_privs, THP disable, timing, timer slack, ambient and
 * bounding capabilities, and EINVAL for an unknown option.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

int main(void) {
    int sig = -1;
    CHECK(prctl(PR_SET_PDEATHSIG, SIGUSR2) == 0);
    CHECK(prctl(PR_GET_PDEATHSIG, &sig) == 0 && sig == SIGUSR2);
    CHECK(prctl(PR_SET_PDEATHSIG, 1000) == -1 && errno == EINVAL);
    CHECK(prctl(PR_SET_PDEATHSIG, 0) == 0);

    char name[16] = {0};
    CHECK(prctl(PR_SET_NAME, "prctl_misc_long_name") == 0);
    CHECK(prctl(PR_GET_NAME, name) == 0 && strcmp(name, "prctl_misc_long") == 0); /* 15 chars */
    CHECK(prctl(PR_SET_NAME, (char*)1) == -1 && errno == EFAULT);

    CHECK(prctl(PR_GET_DUMPABLE) == 1);
    CHECK(prctl(PR_SET_DUMPABLE, 2) == -1 && errno == EINVAL);
    CHECK(prctl(PR_SET_DUMPABLE, 0) == 0 && prctl(PR_GET_DUMPABLE) == 0);
    CHECK(prctl(PR_SET_DUMPABLE, 1) == 0);

    CHECK(prctl(PR_SET_TIMING, 1) == -1 && errno == EINVAL);
    CHECK(prctl(PR_SET_THP_DISABLE, 0, 1) == -1 && errno == EINVAL);
    CHECK(prctl(PR_GET_THP_DISABLE, 1) == -1 && errno == EINVAL);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 1) == -1 && errno == EINVAL);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) == -1 && errno == EINVAL);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 0);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
    CHECK(prctl(PR_CAP_AMBIENT, 100, 0, 0, 0) == -1 && errno == EINVAL);
    CHECK(prctl(PR_CAPBSET_READ, 0) == 1);
    CHECK(prctl(PR_GET_TIMERSLACK) > 0);
    CHECK(prctl(0x7fffffff, 1, 0) == -1 && errno == EINVAL); /* unknown option */

    /* without CAP_SETPCAP */
    CHECK(seteuid(65534) == 0);
    CHECK(prctl(PR_SET_SECUREBITS, 0, 0) == -1 && errno == EPERM);
    CHECK(prctl(PR_CAPBSET_DROP, 1) == -1 && errno == EPERM);
    CHECK(seteuid(0) == 0);

    puts("TEST OK");
    return 0;
}
