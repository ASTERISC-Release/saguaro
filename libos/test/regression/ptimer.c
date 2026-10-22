/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Regression test for the POSIX timers: `timer_create`, `timer_settime`, `timer_gettime`,
 * `timer_getoverrun` and `timer_delete` (the raw syscalls, as the kernel defines them; glibc's
 * `timer_*()` wrappers keep their own user-space bookkeeping around the ids).
 *
 * The signal waits go through `sigwaitinfo()` (a blocking syscall): like every other Saguaro
 * signal test, this test must not busy-wait for a signal, because the LibOS delivers pending
 * signals when a thread enters the kernel. */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void fail(const char* msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/* the kernel's `struct sigevent` (uapi) layout */
struct kernel_sigevent {
    union sigval sigev_value;
    int sigev_signo;
    int sigev_notify;
    union {
        int pad[12];
        int tid;
    } un;
};

#define SIGEV_SIGNAL_UAPI 0
#define SIGEV_NONE_UAPI   1
#define SIGEV_THREAD_UAPI 2

/* the kernel ABI type of a timer id (`__kernel_timer_t`) is a 32-bit int */
static int timer_create_raw(int clock, const struct kernel_sigevent* ev, int* id) {
    return (int)syscall(__NR_timer_create, clock, ev, id);
}
static int timer_settime_raw(int id, int flags, const struct itimerspec* new_val,
                             struct itimerspec* old_val) {
    return (int)syscall(__NR_timer_settime, id, flags, new_val, old_val);
}
static int timer_gettime_raw(int id, struct itimerspec* curr) {
    return (int)syscall(__NR_timer_gettime, id, curr);
}
static long timer_getoverrun_raw(int id) {
    return syscall(__NR_timer_getoverrun, id);
}
static int timer_delete_raw(int id) {
    return (int)syscall(__NR_timer_delete, id);
}

static void expect_errno(const char* what, long ret, int expected) {
    if (ret != -1 || errno != expected) {
        fprintf(stderr, "%s did not fail with %s (ret=%ld errno=%d: %s)\n", what,
                strerror(expected), ret, errno, strerror(errno));
        exit(1);
    }
}

int main(void) {
    struct kernel_sigevent sev = {0};
    int id = 0;
    struct itimerspec its;
    struct itimerspec out;

    /* --- the id is written as 32 bits: a pointer-sized write smashed LTP timer_delete01's stack --- */
    struct {
        int id;
        int canary;
    } slot = {.id = 0, .canary = 0x5a5a5a5a};
    sev.sigev_notify = SIGEV_SIGNAL_UAPI;
    sev.sigev_signo = SIGRTMIN;
    if (timer_create_raw(CLOCK_MONOTONIC, &sev, &slot.id))
        fail("timer_create(slot)");
    if (slot.canary != 0x5a5a5a5a)
        fail("timer_create wrote past the 32-bit timer id");
    if (timer_delete_raw(slot.id))
        fail("timer_delete(slot)");

    /* --- argument validation --- */

    sev.sigev_notify = SIGEV_SIGNAL_UAPI;
    sev.sigev_signo = SIGRTMIN;
    expect_errno("timer_create(-1)", timer_create_raw(-1, &sev, &id), EINVAL);
    expect_errno("timer_create(999)", timer_create_raw(999, &sev, &id), EINVAL);
    expect_errno("timer_create(NULL timerid)",
                 timer_create_raw(CLOCK_MONOTONIC, &sev, NULL), EFAULT);

    struct kernel_sigevent bad_notify = {0};
    bad_notify.sigev_notify = 4242;
    expect_errno("timer_create(bad notify)",
                 timer_create_raw(CLOCK_MONOTONIC, &bad_notify, &id), EINVAL);

    struct kernel_sigevent bad_signo = {0};
    bad_signo.sigev_notify = SIGEV_SIGNAL_UAPI;
    bad_signo.sigev_signo = 12345;
    expect_errno("timer_create(bad signo)",
                 timer_create_raw(CLOCK_MONOTONIC, &bad_signo, &id), EINVAL);

    struct kernel_sigevent thread_notify = {0};
    thread_notify.sigev_notify = SIGEV_THREAD_UAPI;
    expect_errno("timer_create(SIGEV_THREAD)",
                 timer_create_raw(CLOCK_MONOTONIC, &thread_notify, &id), EINVAL);

    /* invalid timer ids are rejected by the other four calls */
    memset(&its, 0, sizeof(its));
    expect_errno("timer_settime(bogus id)",
                 timer_settime_raw(0xdead, 0, &its, NULL), EINVAL);
    expect_errno("timer_gettime(bogus id)", timer_gettime_raw(0xdead, &out), EINVAL);
    expect_errno("timer_getoverrun(bogus id)", timer_getoverrun_raw(0xdead), EINVAL);
    expect_errno("timer_delete(bogus id)", timer_delete_raw(0xdead), EINVAL);

    /* a just-created timer is disarmed */
    if (timer_create_raw(CLOCK_MONOTONIC, &sev, &id)) {
        fail("timer_create failed");
    }
    if (timer_gettime_raw(id, &out)) {
        fail("timer_gettime failed");
    }
    if (out.it_value.tv_sec || out.it_value.tv_nsec) {
        fail("a fresh timer is not disarmed");
    }
    if (timer_delete_raw(id)) {
        fail("timer_delete failed");
    }

    /* --- the NULL `evp` default is SIGEV_SIGNAL of SIGALRM --- */

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    sigaddset(&mask, SIGRTMIN);
    sigaddset(&mask, SIGRTMIN + 1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL)) {
        fail("sigprocmask failed");
    }

    if (timer_create_raw(CLOCK_REALTIME, /*evp=*/NULL, &id)) {
        fail("timer_create(NULL evp) failed");
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 20 * 1000 * 1000;
    if (timer_settime_raw(id, 0, &its, NULL)) {
        fail("timer_settime (default signal) failed");
    }
    siginfo_t got;
    if (sigwaitinfo(&mask, &got) == -1) {
        fail("sigwaitinfo(SIGALRM) failed");
    }
    if (got.si_signo != SIGALRM) {
        fail("the default notification is not SIGALRM");
    }
    if (timer_delete_raw(id)) {
        fail("timer_delete (default signal) failed");
    }

    /* --- one-shot with si_value and SI_TIMER --- */

    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_int = 0x5a;
    if (timer_create_raw(CLOCK_MONOTONIC, &sev, &id)) {
        fail("timer_create (one-shot) failed");
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 50 * 1000 * 1000;
    if (timer_settime_raw(id, 0, &its, NULL)) {
        fail("timer_settime (one-shot) failed");
    }

    if (timer_gettime_raw(id, &out)) {
        fail("timer_gettime (one-shot) failed");
    }
    if (out.it_value.tv_sec != 0 || out.it_value.tv_nsec == 0 ||
            out.it_value.tv_nsec > 50 * 1000 * 1000) {
        fprintf(stderr, "timer_gettime() reports a bad remaining time: %ld.%09ld\n",
                (long)out.it_value.tv_sec, (long)out.it_value.tv_nsec);
        exit(1);
    }

    if (sigwaitinfo(&mask, &got) == -1) {
        fail("sigwaitinfo (one-shot) failed");
    }
    if (got.si_signo != SIGRTMIN || got.si_code != SI_TIMER ||
            got.si_value.sival_int != 0x5a) {
        fprintf(stderr, "the timer signal is wrong: signo=%d code=%d value=%d\n",
                got.si_signo, got.si_code, got.si_value.sival_int);
        exit(1);
    }
    if (timer_getoverrun_raw(id) != 0) {
        fail("a timely one-shot must not report overruns");
    }

    /* after the one-shot fired, the timer is disarmed again */
    if (timer_gettime_raw(id, &out)) {
        fail("timer_gettime (after the one-shot) failed");
    }
    if (out.it_value.tv_sec || out.it_value.tv_nsec) {
        fail("the fired one-shot is still armed");
    }
    if (timer_delete_raw(id)) {
        fail("timer_delete (one-shot) failed");
    }

    /* --- periodic timer: three expirations, then disarm --- */

    sev.sigev_signo = SIGRTMIN + 1;
    sev.sigev_value.sival_int = 0;
    if (timer_create_raw(CLOCK_MONOTONIC, &sev, &id)) {
        fail("timer_create (periodic) failed");
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 20 * 1000 * 1000;
    its.it_interval.tv_nsec = 20 * 1000 * 1000;
    if (timer_settime_raw(id, 0, &its, NULL)) {
        fail("timer_settime (periodic) failed");
    }
    for (int i = 0; i < 3; i++) {
        if (sigwaitinfo(&mask, &got) == -1) {
            fail("sigwaitinfo (periodic) failed");
        }
        if (got.si_signo != SIGRTMIN + 1 || got.si_code != SI_TIMER) {
            fail("not the periodic timer signal");
        }
    }
    if (timer_gettime_raw(id, &out)) {
        fail("timer_gettime (periodic) failed");
    }
    if (out.it_interval.tv_nsec != 20 * 1000 * 1000) {
        fail("timer_gettime() lost the interval");
    }
    memset(&its, 0, sizeof(its));
    if (timer_settime_raw(id, 0, &its, NULL)) { /* disarm */
        fail("timer_settime (disarm) failed");
    }
    if (timer_gettime_raw(id, &out)) {
        fail("timer_gettime (after disarm) failed");
    }
    if (out.it_value.tv_sec || out.it_value.tv_nsec) {
        fail("still armed after disarming");
    }
    if (timer_delete_raw(id)) {
        fail("timer_delete (periodic) failed");
    }

    /* --- overruns: a `SIGEV_NONE` timer armed far in the past --- */

    struct kernel_sigevent none_sev = {.sigev_notify = SIGEV_NONE_UAPI};
    if (timer_create_raw(CLOCK_REALTIME, &none_sev, &id)) {
        fail("timer_create (SIGEV_NONE) failed");
    }
    memset(&its, 0, sizeof(its));
    if (clock_gettime(CLOCK_REALTIME, &its.it_value)) {
        fail("clock_gettime failed");
    }
    its.it_value.tv_sec -= 1;                   /* one second in the past */
    its.it_interval.tv_nsec = 10 * 1000 * 1000; /* 10 ms */
    if (timer_settime_raw(id, 1 /*TIMER_ABSTIME*/, &its, NULL)) {
        fail("timer_settime (abstime in the past) failed");
    }
    long overrun = timer_getoverrun_raw(id);
    if (overrun < 50) { /* about 100 expirations, minus scheduling slack */
        fprintf(stderr, "expected many overruns, got %ld\n", overrun);
        exit(1);
    }
    if (timer_getoverrun_raw(id) != 0) {
        fail("the overrun counter was not cleared");
    }
    memset(&its, 0, sizeof(its));
    if (timer_settime_raw(id, 0, &its, NULL)) {
        fail("timer_settime (stop the overrun timer) failed");
    }
    if (timer_delete_raw(id)) {
        fail("timer_delete (SIGEV_NONE) failed");
    }

    /* deleting twice fails (the id is not valid anymore) */
    if (timer_create_raw(CLOCK_MONOTONIC, &none_sev, &id)) {
        fail("timer_create (twice) failed");
    }
    if (timer_delete_raw(id)) {
        fail("timer_delete (twice, first) failed");
    }
    expect_errno("timer_delete(deleted id)", timer_delete_raw(id), EINVAL);

    printf("TEST OK\n");
    return 0;
}
