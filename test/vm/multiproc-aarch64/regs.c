/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * regs -- the AArch64 syscall ABI keeps every register except x0 across `svc`. This probe loads
 * known values into x6, x7 and x9..x17 (x1..x5 and x8 carry the arguments and the number, so
 * they are compared too), makes a raw `svc`, and checks every one of them afterwards. Cases:
 * getpid (fast), a blocking nanosleep and pipe read (the thread is scheduled out), a
 * signal-interrupted nanosleep (EINTR), fork (the check runs in the parent and in the child),
 * and a signal handler that runs at the return of a getpid loop (the registers must survive the
 * handler and rt_sigreturn). An asynchronous signal to code that makes no system calls is not
 * delivered by the VM PAL at all, so it is not tested. Prints "regs: N/N passed". With the argument
 * `nofork` (the manifest key off: fork is not available) the pipe, fork and wait4 cases are skipped.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NREG 12 /* x6 x7 x9 x10 x11 x12 x13 x14 x15 x16 x17 + x8 */
static const char* const NAMES[NREG] = {"x6", "x7", "x8", "x9", "x10", "x11",
                                        "x12", "x13", "x14", "x15", "x16", "x17"};

static uint64_t pattern(int round, int i) {
    return 0x9e3779b97f4a7c15ULL * (uint64_t)(round * 31 + i + 1) ^ 0x0123456789abcdefULL;
}

/* Raw svc with x6, x7, x9..x17 set from `in` (x8 = number, x0..x5 = args), reading all back. */
static long raw_svc(long nr, long a0, long a1, long a2, long a3, long a4, long a5,
                    const uint64_t in[NREG], uint64_t out[NREG]) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = nr;
    register const uint64_t* pin __asm__("x19") = in;
    register uint64_t* pout __asm__("x20") = out;
    __asm__ volatile(
        "ldp x6, x7, [x19, #0]\n\t"
        "ldp x9, x10, [x19, #24]\n\t"
        "ldp x11, x12, [x19, #40]\n\t"
        "ldp x13, x14, [x19, #56]\n\t"
        "ldp x15, x16, [x19, #72]\n\t"
        "ldr x17, [x19, #88]\n\t"
        "svc #0\n\t"
        "stp x6, x7, [x20, #0]\n\t"
        "str x8, [x20, #16]\n\t"
        "stp x9, x10, [x20, #24]\n\t"
        "stp x11, x12, [x20, #40]\n\t"
        "stp x13, x14, [x20, #56]\n\t"
        "stp x15, x16, [x20, #72]\n\t"
        "str x17, [x20, #88]\n\t"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x8)
        : "r"(pin), "r"(pout)
        : "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory",
          "cc");
    return x0;
}

static int g_total, g_pass, g_msgs;

static int compare(const char* what, const uint64_t in[NREG], const uint64_t out[NREG], long nr) {
    int bad = 0;
    for (int i = 0; i < NREG; i++) {
        uint64_t want = (i == 2) ? (uint64_t)nr : in[i];
        if (out[i] != want) {
            if (g_msgs++ < 4)
            printf("regs: %s: %s is 0x%lx, expected 0x%lx\n", what, NAMES[i], (unsigned long)out[i],
                   (unsigned long)want);
            bad = 1;
        }
    }
    return bad;
}

static void report(const char* what, int bad) {
    g_total++;
    g_msgs = 0;
    if (!bad)
        g_pass++;
    printf("regs: %-14s %s\n", what, bad ? "FAIL" : "ok");
    fflush(stdout);
}

static void make_in(int round, uint64_t in[NREG]) {
    for (int i = 0; i < NREG; i++)
        in[i] = pattern(round, i);
}

static void case_simple(const char* what, long nr, long a0, long a1, long a2, long a3, int rounds) {
    int bad = 0;
    for (int r = 0; r < rounds; r++) {
        uint64_t in[NREG], out[NREG];
        make_in(r, in);
        raw_svc(nr, a0, a1, a2, a3, 0, 0, in, out);
        bad |= compare(what, in, out, nr);
    }
    report(what, bad);
}

static volatile sig_atomic_t g_got;
static void on_alarm(int sig) { (void)sig; g_got++; }

static void arm_timer(int usec) {
    struct itimerval it = {.it_value = {.tv_sec = 0, .tv_usec = usec}};
    setitimer(ITIMER_REAL, &it, NULL);
}

int main(int argc, char** argv) {
    int forks = !(argc > 1 && !strcmp(argv[1], "nofork")); /* key off: no fork, no pipe peer */
    struct sigaction sa = {.sa_handler = on_alarm};
    sigaction(SIGALRM, &sa, NULL);

    case_simple("getpid", SYS_getpid, 0, 0, 0, 0, 500);
    case_simple("sched_yield", SYS_sched_yield, 0, 0, 0, 0, 200);

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 2 * 1000 * 1000};
    case_simple("nanosleep", SYS_nanosleep, (long)&ts, 0, 0, 0, 20);

    int fds[2];
    if (forks && pipe(fds) < 0) return 2;
    pid_t writer = forks ? fork() : -1;
    if (forks && writer == 0) {
        for (int i = 0; i < 20; i++) {
            usleep(3000);
            if (write(fds[1], "x", 1) != 1) _exit(1);
        }
        _exit(0);
    }
    if (forks) {
        int bad = 0;
        char c;
        for (int r = 0; r < 20; r++) {
            uint64_t in[NREG], out[NREG];
            make_in(r, in);
            long n = raw_svc(SYS_read, fds[0], (long)&c, 1, 0, 0, 0, in, out);
            bad |= (n != 1) | compare("pipe read", in, out, SYS_read);
        }
        report("pipe read", bad);
        waitpid(writer, NULL, 0);
    }

    {   /* a signal interrupts a blocking call */
        int bad = 0;
        struct timespec long_ts = {.tv_sec = 5};
        for (int r = 0; r < 10; r++) {
            uint64_t in[NREG], out[NREG];
            make_in(r, in);
            g_got = 0;
            arm_timer(4000);
            long ret = raw_svc(SYS_nanosleep, (long)&long_ts, 0, 0, 0, 0, 0, in, out);
            bad |= (ret != -EINTR) | (g_got != 1) | compare("EINTR nanosleep", in, out, SYS_nanosleep);
        }
        report("EINTR nanosleep", bad);
    }

    if (forks) {   /* fork: parent and child both check */
        int bad = 0;
        for (int r = 0; r < 10; r++) {
            uint64_t in[NREG], out[NREG];
            make_in(r, in);
            fflush(stdout);
            long pid = raw_svc(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0, in, out);
            if (pid == 0)
                _exit(compare("fork child", in, out, SYS_clone) ? 1 : 0);
            int status = 0;
            waitpid(pid, &status, 0);
            bad |= (pid < 0) | !WIFEXITED(status) | (WEXITSTATUS(status) != 0) |
                   compare("fork parent", in, out, SYS_clone);
        }
        report("fork", bad);
    }

    if (forks) {   /* wait4 that blocks until the child exits */
        int bad = 0;
        for (int r = 0; r < 10; r++) {
            pid_t pid = fork();
            if (pid == 0) { usleep(3000); _exit(7); }
            uint64_t in[NREG], out[NREG];
            make_in(r, in);
            int status = 0;
            long ret = raw_svc(SYS_wait4, pid, (long)&status, 0, 0, 0, 0, in, out);
            bad |= (ret != pid) | (WEXITSTATUS(status) != 7) | compare("wait4", in, out, SYS_wait4);
        }
        report("wait4", bad);
    }

    {   /* a signal that arrives while the code runs is delivered when a system call returns:
         * the handler runs there, and the registers of that call must be back afterwards */
        int bad = 0;
        for (int r = 0; r < 10; r++) {
            uint64_t in[NREG], out[NREG];
            make_in(r, in);
            g_got = 0;
            arm_timer(4000);
            long spins = 0;
            while (!g_got) {
                raw_svc(SYS_getpid, 0, 0, 0, 0, 0, 0, in, out);
                bad |= compare("signal at return", in, out, SYS_getpid);
                if (++spins > 100000000) { bad = 1; break; }
            }
        }
        report("signal at return", bad);
    }

    printf("regs: %d/%d passed (x6 x7 x8 x9..x17 across svc, blocking, EINTR, %sfork, signal at return)\n",
           g_pass, g_total, forks ? "" : "no ");
    return g_pass == g_total ? 0 : 1;
}
