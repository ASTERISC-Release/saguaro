/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * efault.c -- faults of the LibOS on application memory (it runs at EL1 under the AArch64 VM
 * PAL). Each case is run in a child process when fork() works (the parent reports how the child
 * ended), else in this process (the guest's exit tells).
 *
 *   sigframe   a signal handler that raises its own signal again (SA_NODEFER) until the signal
 *              frames run off the end of the stack: the LibOS writes the next frame into unmapped
 *              memory (LTP kill10, mremap01, munmap02, prot_hsymlinks on the AArch64 matrix).
 *              Linux kills the process with SIGSEGV; the LibOS reports "Internal memory fault"
 *              and the process exits; before phase 4 the AArch64 VM PAL panicked the guest.
 *   altstack   the same with the handler on a small sigaltstack whose next frame does not fit.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile long g_depth;

static void recurse(int sig) {
    g_depth++;
    if (g_depth % 1000 == 0) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "depth %ld\n", g_depth);
        if (write(1, buf, n) < 0)
            _exit(3);
    }
    raise(sig);
}

static int run(const char* name) {
    g_depth = 0;
    struct sigaction sa = {.sa_handler = recurse, .sa_flags = SA_NODEFER};
    if (!strcmp(name, "altstack")) {
        stack_t ss = {.ss_size = 16384};
        ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                        0);
        sigaltstack(&ss, NULL);
        sa.sa_flags |= SA_ONSTACK;
    }
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    printf("%s: returned (unexpected)\n", name);
    return 1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* cases[] = {"sigframe", "altstack", NULL};
    int nofork = getenv("EFAULT_NOFORK") != NULL;
    for (int i = 0; cases[i]; i++) {
        if (argc > 1 && strcmp(argv[1], cases[i]))
            continue;
        pid_t pid = nofork ? -1 : fork();
        if (pid == 0)
            return run(cases[i]);
        if (pid < 0) {
            printf("%s: in this process (no fork)\n", cases[i]);
            return run(cases[i]);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status))
            printf("%s: child killed by signal %d\n", cases[i], WTERMSIG(status));
        else
            printf("%s: child exited with %d\n", cases[i], WEXITSTATUS(status));
    }
    return 0;
}
