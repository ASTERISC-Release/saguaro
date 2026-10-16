/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * vk-two: several graphics processes in one guest (Documentation/devel/proton-vm/design-gpu.md,
 * "Several graphics processes"). Starts N children (default 2, argument 1) at once, each an exec
 * of the program named by argument 2 (default /vk-headless) with the remaining arguments, and
 * reports their exit codes: every child must start or join the guest's one graphics session and
 * run to completion. Under gramine-vm with `make vm-vk-two.manifest MULTIPROCESS=1 TWO=1` the
 * children are demand-paged processes with their own LibOS; before the shared session, the second
 * child spun for ever in setup_syscall_comms().
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 2;
    const char* prog = argc > 2 ? argv[2] : "/vk-headless";
    char** child_argv = calloc((size_t)(argc > 3 ? argc - 2 : 1) + 1, sizeof(char*));
    if (!child_argv)
        return 1;
    child_argv[0] = (char*)prog;
    for (int i = 3; i < argc; i++)
        child_argv[i - 2] = argv[i];

    pid_t pids[64];
    if (n < 1 || n > 64) {
        fprintf(stderr, "vk-two: 1 to 64 children\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            return 1;
        }
        if (pids[i] == 0) {
            execv(prog, child_argv);
            fprintf(stderr, "vk-two: exec %s: %s\n", prog, strerror(errno));
            _exit(127);
        }
        printf("vk-two: child %d pid %d: %s\n", i, pids[i], prog);
        fflush(stdout);
    }

    int failed = 0;
    for (int i = 0; i < n; i++) {
        int status = 0;
        if (waitpid(pids[i], &status, 0) < 0) {
            perror("waitpid");
            failed++;
            continue;
        }
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        printf("vk-two: child %d pid %d exited %d%s\n", i, pids[i], code,
               WIFSIGNALED(status) ? " (signal)" : "");
        fflush(stdout);
        if (code != 0)
            failed++;
    }
    printf("vk-two: %d of %d children ok\n", n - failed, n);
    return failed ? 1 : 0;
}
