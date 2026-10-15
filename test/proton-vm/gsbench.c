/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Costs that decide the `%gs` design (Documentation/devel/proton-vm/design-gs.md): what a system
 * call costs today on the fast path (glibc's `getppid`) and on a raw `syscall` instruction, what
 * the swap of the GS base would add in user mode (`rdgsbase` + `wrgsbase`, needs CR4.FSGSBASE) or
 * through the kernel (`arch_prctl(ARCH_SET_GS)` natively; under Gramine it is ENOSYS, so
 * `ARCH_SET_FS` with the current value stands in), and a plain TLS load through `%fs`.
 * Usage: gsbench [iterations] (default 1,000,000). Run natively, under gramine-direct and
 * gramine-vm; the `wrgsbase` row needs FSGSBASE (it is skipped when the instruction faults).
 */

#define _GNU_SOURCE
#include <asm/prctl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static sigjmp_buf g_jmp;
static void on_sigill(int sig) {
    (void)sig;
    siglongjmp(g_jmp, 1);
}

static __thread uint64_t g_tls_slot = 42;

#define ROW(name, iters, expr)                                        \
    do {                                                              \
        double t0 = now_ns();                                         \
        for (long i_ = 0; i_ < (iters); i_++) {                       \
            expr;                                                     \
        }                                                             \
        double dt = now_ns() - t0;                                    \
        printf("%-44s %8.1f ns\n", name, dt / (iters));               \
    } while (0)

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* the VM guest dies at wrgsbase when FSGSBASE is off */
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    volatile uint64_t sink = 0;

    ROW("getppid() (glibc fast path or syscall)", n, sink += getppid());
    ROW("raw syscall getppid", n, ({ long r; __asm__ volatile("syscall" : "=a"(r) : "0"(SYS_getppid) : "rcx", "r11", "memory"); sink += r; }));
    ROW("TLS load through %fs", n, sink += g_tls_slot);

    unsigned long fs = 0;
    syscall(SYS_arch_prctl, ARCH_GET_FS, &fs);
    ROW("arch_prctl(ARCH_SET_FS, same) (the kernel way)", n / 10, syscall(SYS_arch_prctl, ARCH_SET_FS, fs));

    signal(SIGILL, on_sigill);
    if (sigsetjmp(g_jmp, 1) == 0) {
        uint64_t saved;
        __asm__ volatile("rdgsbase %0" : "=r"(saved));
        ROW("rdgsbase + wrgsbase (user mode, FSGSBASE)", n, ({ uint64_t v; __asm__ volatile("rdgsbase %0\n\twrgsbase %0" : "=r"(v) :: "memory"); sink += v; }));
        ROW("rdgsbase+wrgsbase x2 + TLS load (one swap round trip)", n, ({ uint64_t v; __asm__ volatile("rdgsbase %0\n\twrgsbase %0\n\trdgsbase %0\n\twrgsbase %0" : "=r"(v) :: "memory"); sink += v + g_tls_slot; }));
        __asm__ volatile("wrgsbase %0" ::"r"(saved));
    } else {
        printf("%-44s %s\n", "rdgsbase + wrgsbase (user mode, FSGSBASE)", "SIGILL: FSGSBASE off");
    }
    return sink == 1 ? 1 : 0;
}
