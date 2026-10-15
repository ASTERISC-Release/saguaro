/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Reproducer for the vbs PAL (platform-issues.md): asynchronous signal delivery. A 10 ms ITIMER_REAL
 * timer runs while the thread (a) computes for one second without system calls and then (b) makes a
 * cheap system call in every iteration for one second. Under Linux (and the Linux PAL) both phases
 * see about 100 SIGALRMs; in a VBS enclave nothing can interrupt a thread that runs enclave code, so
 * phase (a) sees the signals only when it makes its next system call.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile long g_count;

static void handler(int sig) {
    (void)sig;
    g_count++;
}

/* reads the time without a system call (the counter register); one second of spinning */
static unsigned long cntvct(void) {
    unsigned long v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static unsigned long cntfrq(void) {
    unsigned long v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

int main(void) {
    struct sigaction sa = {.sa_handler = handler};
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {.it_interval = {0, 10000}, .it_value = {0, 10000}};
    setitimer(ITIMER_REAL, &it, NULL);

    unsigned long hz = cntfrq(), end = cntvct() + hz;
    while (cntvct() < end)
        ;
    long during_a = g_count;
    getppid(); /* the first system call after phase (a) */
    long after_a = g_count;

    end = cntvct() + hz;
    while (cntvct() < end)
        getppid();
    long b = g_count - after_a;

    printf("phase a (no system calls): %ld signals while spinning, %ld after the next syscall\n",
           during_a, after_a - during_a);
    printf("phase b (a system call per iteration): %ld signals\n", b);
    if (b >= 50)
        printf("TEST OK\n");
    return 0;
}
