/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Faults and signals inside a VBS enclave (phase 3b): synchronous faults of the application are
 * delivered to its signal handlers through Windows SEH -> the vbs PAL -> the LibOS.
 *   (no argument)  SIGSEGV for a NULL read, a write to a read-only page, a read of a PROT_NONE
 *                  page (handler leaves with siglongjmp); SIGILL/SIGTRAP for a trap instruction;
 *                  a handler that makes the page writable and returns (the faulting store is
 *                  then retried); a signal sent to itself. Exit code 0 if all passed.
 *   exec-nx        jump into a read-write data page (phase 0: such a fault may hang the thread)
 *   write-code     write to the program's own code (phase 0: may hang)
 */

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf g_jmp;
static volatile int g_signo;
static void* volatile g_addr;
static char* g_fix_page;

static void on_signal(int signo, siginfo_t* si, void* uc) {
    (void)uc;
    g_signo = signo;
    g_addr = si->si_addr;
    siglongjmp(g_jmp, 1);
}

static void on_signal_fix(int signo, siginfo_t* si, void* uc) {
    (void)uc;
    g_signo = signo;
    g_addr = si->si_addr;
    mprotect(g_fix_page, 4096, PROT_READ | PROT_WRITE); /* repair and return: the store retries */
}

static volatile int g_usr1;
static void on_usr1(int signo) {
    g_usr1 = signo;
}

static void handle(int signo, void (*h)(int, siginfo_t*, void*)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, NULL);
}

static int check(const char* what, int want_sig, void* want_addr) {
    int ok = g_signo == want_sig && (!want_addr || g_addr == want_addr);
    printf("%-34s signal %d (want %d), si_addr %p (want %p): %s\n", what, g_signo, want_sig,
           g_addr, want_addr, ok ? "ok" : "FAILED");
    g_signo = 0;
    g_addr = NULL;
    return !ok;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fails = 0;
    handle(SIGSEGV, on_signal);
    handle(SIGILL, on_signal);
    handle(SIGTRAP, on_signal);
    handle(SIGBUS, on_signal);

    char* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        printf("mmap failed\n");
        return 1;
    }

    if (argc > 1 && !strcmp(argv[1], "exec-nx")) {
        printf("jumping into a read-write data page...\n");
        if (!sigsetjmp(g_jmp, 1))
            ((void (*)(void))page)();
        return check("exec-nx", SIGSEGV, NULL);
    }
    if (argc > 1 && !strcmp(argv[1], "write-code")) {
        printf("writing to the program's own code...\n");
        if (!sigsetjmp(g_jmp, 1))
            *(volatile int*)(void*)&main = 0;
        return check("write-code", SIGSEGV, (void*)&main);
    }

    const char* only = argc > 1 ? argv[1] : NULL;
#define RUN(name) (!only || strstr(only, name))

    if (RUN("null")) {
        printf("-- NULL read\n");
        if (!sigsetjmp(g_jmp, 1))
            g_signo = *(volatile int*)0;
        fails += check("NULL read", SIGSEGV, (void*)0);
    }

    if (RUN("trap")) {
        printf("-- trap instruction\n");
        if (!sigsetjmp(g_jmp, 1))
            __builtin_trap();
        if (g_signo == SIGTRAP)
            g_signo = SIGILL; /* either is a faithful report of a trap instruction */
        fails += check("trap instruction", SIGILL, NULL);
    }

    if (RUN("usr1")) {
        printf("-- kill(self, SIGUSR1)\n");
        signal(SIGUSR1, on_usr1);
        kill(getpid(), SIGUSR1);
        printf("%-34s handler ran: %s\n", "kill(self, SIGUSR1)", g_usr1 == SIGUSR1 ? "ok" : "FAILED");
        fails += g_usr1 != SIGUSR1;
    }

    if (RUN("none")) {
        printf("-- read of a PROT_NONE page\n");
        mprotect(page, 4096, PROT_NONE);
        if (!sigsetjmp(g_jmp, 1))
            g_signo = *(volatile char*)(page + 16);
        fails += check("read of a PROT_NONE page", SIGSEGV, page + 16);
    }

    if (RUN("ro")) {
        printf("-- write to a read-only page\n");
        mprotect(page, 4096, PROT_READ);
        if (!sigsetjmp(g_jmp, 1))
            *(volatile char*)(page + 8) = 1;
        fails += check("write to a read-only page", SIGSEGV, page + 8);
    }

    if (RUN("fix")) {
        printf("-- handler repairs the page and returns\n");
        g_fix_page = page;
        mprotect(page, 4096, PROT_READ);
        handle(SIGSEGV, on_signal_fix);
        *(volatile char*)(page + 32) = 42; /* faults once; the handler repairs it and returns */
        fails += check("handler returns, store retried", SIGSEGV, page + 32);
        if (page[32] != 42) {
            printf("  the retried store did not happen (page[32] = %d)\n", page[32]);
            fails++;
        }
    }

    printf("signals test: %s\n", fails ? "FAILED" : "passed");
    return fails;
}
