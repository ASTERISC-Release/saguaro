/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * An asynchronous signal can interrupt the application at *any* instruction, including ones
 * where the intra-procedure-call registers x16/x17 or the condition flags are live (PLT stubs,
 * veneers, a `cmp` followed by its conditional branch). Returning from the signal handler must
 * restore the interrupted context exactly. Linux does this in the kernel; a user-space
 * implementation needs care because the final jump needs a scratch register.
 *
 * Two loops, each interrupted at high rate by signals sent from a helper thread:
 *  - x17 stays live across a run of nops and is checked afterwards;
 *  - the Z flag is set by `cmp` and consumed by `b.ne` after a run of nops.
 * A single mismatch fails the test. aarch64 only (the register/flag details are architectural).
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_long g_signals;

static void handler(int sig) {
    (void)sig;
    atomic_fetch_add(&g_signals, 1);
    /* Clobber the flags and caller-saved registers in the handler on purpose. */
    volatile unsigned long x = 0;
    for (int i = 0; i < 50; i++)
        x += (unsigned long)i * 3;
}

#ifdef __aarch64__
#define MAGIC 0x5a5a5a5a5a5aUL

/* Returns 0 on success, 1 if x17 was not preserved. */
static int x17_loop(long iters) {
    int bad;
    __asm__ volatile(
        "mov x16, %[n]\n"
        "1:\n"
        "mov x17, %[m]\n"
        ".rept 24\n nop\n .endr\n"
        "cmp x17, %[m]\n"
        "b.ne 2f\n"
        "subs x16, x16, #1\n"
        "b.ne 1b\n"
        "mov %w[bad], #0\n"
        "b 3f\n"
        "2: mov %w[bad], #1\n"
        "3:\n"
        : [bad] "=r"(bad)
        : [n] "r"(iters), [m] "r"(MAGIC)
        : "x16", "x17", "cc", "memory");
    return bad;
}

/* Returns 0 on success, 1 if the Z flag (set by cmp) did not survive until the b.ne. */
static int flags_loop(long iters) {
    int bad;
    __asm__ volatile(
        "mov x16, %[n]\n"
        "mov x15, #7\n"
        "1:\n"
        "cmp x15, #7\n"          /* Z = 1 */
        ".rept 24\n nop\n .endr\n"
        "b.ne 2f\n"              /* must not be taken */
        "subs x16, x16, #1\n"
        "b.ne 1b\n"
        "mov %w[bad], #0\n"
        "b 3f\n"
        "2: mov %w[bad], #1\n"
        "3:\n"
        : [bad] "=r"(bad)
        : [n] "r"(iters)
        : "x15", "x16", "cc", "memory");
    return bad;
}
#endif

static atomic_int g_stop;
static pthread_t g_main_thread;

static void* killer(void* arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        pthread_kill(g_main_thread, SIGUSR1);
        struct timespec ts = {0, 20000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(void) {
#ifdef __aarch64__
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }
    g_main_thread = pthread_self();
    pthread_t k;
    if (pthread_create(&k, NULL, killer, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    /* Run until enough signals were handled (or a generous time limit), checking after each batch. */
    const long target_signals = 3000;
    time_t deadline = time(NULL) + 60;
    int bad_x17 = 0, bad_flags = 0;
    while (atomic_load(&g_signals) < target_signals && time(NULL) < deadline && !bad_x17
           && !bad_flags) {
        bad_x17 = x17_loop(20000);
        bad_flags = flags_loop(20000);
    }
    atomic_store(&g_stop, 1);
    pthread_join(k, NULL);

    printf("signals handled: %ld\n", atomic_load(&g_signals));
    if (bad_x17) {
        printf("FAIL: x17 not preserved across an asynchronous signal\n");
        return 1;
    }
    if (bad_flags) {
        printf("FAIL: condition flags not preserved across an asynchronous signal\n");
        return 1;
    }
    if (atomic_load(&g_signals) < target_signals / 10) {
        printf("too few signals delivered to make the test meaningful\n");
        return 1;
    }
#else
    printf("(not aarch64: nothing to check)\n");
#endif
    printf("TEST OK\n");
    return 0;
}
