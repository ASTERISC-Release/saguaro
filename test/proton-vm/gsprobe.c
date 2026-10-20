/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A program that owns %gs, as 64-bit Windows code under Wine does (`NtCurrentTeb()` is
 * `mov %gs:0x30`): it sets a TEB-like block with arch_prctl(ARCH_SET_GS) and checks that
 * `%gs:0x30` stays the block's address across everything the VM PAL's hand-over must survive
 * (dev docs: proton-vm/design-gs.md, stage S1): raw system calls, a blocking read, a
 * spin long enough for timer preemption, a page fault, a signal (inside the handler and after it),
 * a new thread (inherits, then sets its own), a fork (inherits), ARCH_GET_GS, and a
 * clock_gettime through glibc and through the vDSO's entry point. Every other system call here is
 * a raw `syscall` (the glibc of the guest is the host's), so the result tells about the kernel
 * paths. Prints PASS/FAIL lines.
 */

#define _GNU_SOURCE
#include <asm/prctl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct teb {
    uint64_t pad[6];
    struct teb* self; /* offset 0x30, as NtTib.Self */
    uint64_t pad2[9];
};

static long raw(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

static uint64_t gs30(void) {
    uint64_t v;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(v));
    return v;
}

static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)

static struct teb g_teb __attribute__((aligned(64)));
static volatile uint64_t g_in_handler;
static void handler(int sig) {
    (void)sig;
    g_in_handler = gs30();
}

static struct teb g_thread_teb;
static void* thread_fn(void* arg) {
    uint64_t inherited = gs30();
    *(uint64_t*)arg = inherited;
    g_thread_teb.self = &g_thread_teb;
    raw(SYS_arch_prctl, ARCH_SET_GS, (long)&g_thread_teb, 0);
    uint64_t own = gs30();
    raw(SYS_nanosleep, (long)&(struct timespec){0, 20000000}, 0, 0);
    return (void*)(uintptr_t)(own == (uint64_t)&g_thread_teb && gs30() == (uint64_t)&g_thread_teb);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_teb.self = &g_teb;

    /* where the vDSO is (Gramine's, in a build with the VM PAL, issues `syscall` and never jumps
     * through %gs: libos/src/vdso/arch/x86_64/meson.build) */
    printf("INFO AT_SYSINFO_EHDR = %p\n", (void*)getauxval(AT_SYSINFO_EHDR));
    long r = raw(SYS_arch_prctl, ARCH_SET_GS, (long)&g_teb, 0);
    printf("arch_prctl(ARCH_SET_GS) = %ld\n", r);
    if (r != 0) {
        printf("FAIL cannot set %%gs (%ld): nothing else to test\n", r);
        return 1;
    }
    CHECK("gs:0x30 right after ARCH_SET_GS", gs30() == (uint64_t)&g_teb);

    unsigned long got = 0;
    r = raw(SYS_arch_prctl, ARCH_GET_GS, (long)&got, 0);
    CHECK("ARCH_GET_GS returns the base", r == 0 && got == (unsigned long)&g_teb);
    CHECK("gs:0x30 after a raw system call", gs30() == (uint64_t)&g_teb);

    /* glibc's clock_gettime: through the vDSO if glibc took it, else a system call */
    struct timespec ts = {0, 0};
    int cr = clock_gettime(CLOCK_MONOTONIC, &ts);
    CHECK("clock_gettime (glibc) works and keeps gs:0x30", cr == 0 && ts.tv_sec > 0 && gs30() == (uint64_t)&g_teb);
    /* the vDSO's own entry point, whether or not glibc chose it: linux-vdso.so.1 is in the link
     * map, so dlsym finds the symbol when the vDSO carries the LINUX_2.6 version */
    int (*vdso_cg)(clockid_t, struct timespec*) = dlsym(RTLD_DEFAULT, "__vdso_clock_gettime");
    if (vdso_cg) {
        ts.tv_sec = 0;
        cr = vdso_cg(CLOCK_MONOTONIC, &ts);
        CHECK("__vdso_clock_gettime called directly works and keeps gs:0x30", cr == 0 && ts.tv_sec > 0 && gs30() == (uint64_t)&g_teb);
    } else {
        printf("INFO __vdso_clock_gettime not found by dlsym (glibc did not take the vDSO)\n");
    }

    /* a blocking read: the thread sleeps in the kernel and is switched out and in */
    int pfd[2];
    raw(SYS_pipe2, (long)pfd, 0, 0);
    pid_t pid = raw(SYS_fork, 0, 0, 0);
    if (pid == 0) {
        raw(SYS_nanosleep, (long)&(struct timespec){0, 50000000}, 0, 0);
        char c = 'x';
        raw(SYS_write, pfd[1], (long)&c, 1);
        raw(SYS_exit_group, gs30() == (uint64_t)&g_teb ? 0 : 1, 0, 0);
    }
    char c = 0;
    raw(SYS_read, pfd[0], (long)&c, 1);
    CHECK("gs:0x30 after a blocking read", c == 'x' && gs30() == (uint64_t)&g_teb);
    int status = -1;
    raw(SYS_wait4, pid, (long)&status, 0);
    CHECK("the forked child inherited gs:0x30", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* spin through several timer ticks (preemption in user mode) */
    volatile uint64_t x = 0;
    uint64_t bad = 0;
    for (int i = 0; i < 300; i++) {
        for (int j = 0; j < 1000000; j++)
            x += j;
        if (gs30() != (uint64_t)&g_teb)
            bad++;
    }
    CHECK("gs:0x30 across 300 ms of user-mode spinning (timer preemption)", bad == 0);

    /* a page fault in user mode */
    char* page = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < 256; i++)
        page[i * 4096] = 1;
    CHECK("gs:0x30 after 256 page faults", gs30() == (uint64_t)&g_teb);

    /* a signal: inside the handler (S3: the LibOS enters the handler through the PAL's stub, which
     * loads the application's base) and after it */
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigaction(SIGUSR1, &sa, NULL);
    g_in_handler = 0;
    pid_t self = raw(SYS_getpid, 0, 0, 0);
    raw(SYS_kill, self, SIGUSR1, 0);
    uint64_t right_after = gs30(); /* before any further system call reloads the base */
    CHECK("gs:0x30 inside the signal handler (the TEB)", g_in_handler == (uint64_t)&g_teb);
    CHECK("gs:0x30 right after the handler returned, before another system call", right_after == (uint64_t)&g_teb);
    /* a fault (not a system call) delivers the next one; the handler changes nothing */
    g_in_handler = 0;
    raw(SYS_kill, self, SIGUSR1, 0);
    CHECK("gs:0x30 after the signal handler returned", gs30() == (uint64_t)&g_teb);

    /* a thread: inherits the creator's base, then sets its own */
    pthread_t t;
    uint64_t inherited = 1;
    void* ret = NULL;
    pthread_create(&t, NULL, thread_fn, &inherited);
    pthread_join(t, &ret);
    CHECK("a new thread inherits gs:0x30", inherited == (uint64_t)&g_teb);
    CHECK("the thread's own gs:0x30 after its ARCH_SET_GS, a sleep", ret == (void*)1);
    CHECK("the main thread's gs:0x30 after the thread ran", gs30() == (uint64_t)&g_teb);

    /* back to no base of its own: the old paths again */
    raw(SYS_arch_prctl, ARCH_SET_GS, 0, 0);
    raw(SYS_arch_prctl, ARCH_GET_GS, (long)&got, 0);
    CHECK("ARCH_SET_GS 0 gives the base up", got == 0);

    printf("%s: %d failure(s)\n", fails ? "GSPROBE FAILED" : "GSPROBE OK", fails);
    return fails ? 1 : 0;
}
