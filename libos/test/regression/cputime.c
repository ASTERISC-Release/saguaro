/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * CPU time accounting (LTP getrusage01-04, times03, clock_gettime01): after half a second of
 * busy work, getrusage(RUSAGE_SELF/RUSAGE_THREAD), times() and the CPU-time clocks report a
 * nonzero CPU time that does not exceed the elapsed wall-clock time much, and the counters
 * never go backwards.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__,        \
                    __LINE__, #cond);                              \
            exit(1);                                               \
        }                                                          \
    } while (0)

static double ts(clockid_t clk) {
    struct timespec t;
    CHECK(clock_gettime(clk, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double ru(int who) {
    struct rusage r;
    CHECK(getrusage(who, &r) == 0);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec +
           r.ru_stime.tv_usec / 1e6;
}

int main(void) {
    double wall0 = ts(CLOCK_MONOTONIC);
    double proc0 = ts(CLOCK_PROCESS_CPUTIME_ID);
    double self0 = ru(RUSAGE_SELF);

    volatile unsigned long x = 0;
    while (ts(CLOCK_MONOTONIC) - wall0 < 0.5)
        for (int i = 0; i < 100000; i++)
            x += i;

    double wall = ts(CLOCK_MONOTONIC) - wall0;
    double proc = ts(CLOCK_PROCESS_CPUTIME_ID) - proc0;
    double self = ru(RUSAGE_SELF) - self0;
    double thread = ru(RUSAGE_THREAD);
    double thread_clock = ts(CLOCK_THREAD_CPUTIME_ID);

    struct tms t;
    CHECK(times(&t) != (clock_t)-1);
    double tms = (double)(t.tms_utime + t.tms_stime) / sysconf(_SC_CLK_TCK);

    printf("wall %.3f proc %.3f self %.3f thread %.3f thread_clock %.3f times %.3f\n", wall, proc,
           self, thread, thread_clock, tms);
    /* at least a fifth of the busy period is ours even on a loaded machine; the CPU time of one
     * thread cannot exceed the wall time by more than the counters' granularity */
    CHECK(proc > wall / 5 && proc < wall + 0.1);
    CHECK(self > wall / 5 && self < wall + 0.1);
    CHECK(thread > wall / 5);
    CHECK(thread_clock > wall / 5);
    CHECK(tms > wall / 5);
    CHECK(t.tms_cutime == 0 && t.tms_cstime == 0);
    puts("TEST OK");
    return 0;
}
