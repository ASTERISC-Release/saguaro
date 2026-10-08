/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Regression test for FEAT_PAuth (branch-target/pac-ret) return addresses across fork().
 *
 * When userspace is built with -mbranch-protection (Ubuntu arm64 gcc default since
 * 23.04; forced for this file via `#pragma GCC target` below), non-leaf functions sign their return
 * address with `paciasp` on entry and authenticate it with `autiasp` in the epilogue;
 * an authentication failure raises SIGILL *at that epilogue*. PAC keys are per host
 * task, and the key only has to "work" within a task -- except across fork(): a native
 * fork() child still authenticates signatures the parent made before the fork (the
 * kernel delivers the child with the parent's keys), so e.g. Ubuntu's bash -- whose
 * `make_child()` returns through exactly such a signature in the vfork child interval
 * -- runs fine natively. Gramine emulates fork() in-process and resumes the child on a
 * Gramine host task, where the stale signature cannot authenticate (no EL0 interface
 * to copy keys exists); `illegal_upcall()`'s failed-authentication repair in
 * libos/src/bookkeep/libos_signal.c emulates the successful-authentication outcome.
 * Without that repair this test dies with SIGILL in the child, exactly like the
 * original bug report (Ubuntu bash: every forked external command "Illegal
 * instruction (core dumped)").
 *
 * On hardware without PAuth the pac/aut instructions are NOPs and on host architectures
 * without branch protection support the pragma degrades this to a plain fork test, so it
 * is safe to run unconditionally.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __aarch64__
/* Force pac-ret even on toolchains that do not enable branch protection by default
 * (Ubuntu's does, which is how the original bug surfaced with distro binaries). */
#pragma GCC target("branch-protection=standard")
#endif

static volatile int sink;
static volatile int is_child;

/*
 * Recursive (hence always a real stack frame with a pac-ret epilogue, even at -O2).
 * fork() at depth 6; the child returns normally through recurse(6)..recurse(1), i.e.
 * through 6 return addresses signed by the parent's task before the fork.
 */
static int recurse(int n) {
    sink += n;
    if (n == 6) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0) {
            is_child = 1;
            return recurse(5) + 1;
        }
    }
    if (n) {
        return recurse(n - 1) + 1;
    }
    return 0;
}

int main(void) {
    int r = recurse(12);

    if (is_child) {
        /* Reached only in the child: it has returned through all pre-fork-signed
         * frames (and through `main`'s own epilogue machinery) without SIGILL. */
        printf("child: returned through pre-fork-signed frames (r=%d)\n", r);
        fflush(stdout);
        _exit(0);
    }

    int status;
    if (wait(&status) < 0) {
        perror("wait");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child died abnormally: status=0x%x (sig=%d)\n", status,
                WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return 1;
    }
    if (r != 12) {
        fprintf(stderr, "unexpected parent r=%d (expected 12)\n", r);
        return 1;
    }
    printf("TEST OK\n");
    return 0;
}
