/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A read running past the end of an application mapping must behave as under Linux: it reads
 * the neighbouring mapping or it is the application's SIGSEGV; it must never end the process
 * because the neighbour is a mapping of the LibOS itself (a thread's LibOS stack is allocated at
 * every pthread_create()). The test interleaves anonymous mappings with thread creation, so that
 * an allocator placing both kinds top-down next to each other would put a LibOS stack right
 * above an application mapping, then reads one byte past every mapping with a SIGSEGV handler
 * armed. ASLR is off in its manifest so that the layout is deterministic.
 */

#define _GNU_SOURCE
#include <err.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAPPINGS 8
#define MAPPING_SIZE (1u << 20)

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_faults;
static pthread_barrier_t g_done;

static void on_segv(int sig, siginfo_t* si, void* ctx) {
    (void)sig;
    (void)si;
    (void)ctx;
    g_faults++;
    siglongjmp(g_jmp, 1);
}

static void* thread_fn(void* arg) {
    (void)arg;
    /* stays alive (and so does its LibOS stack) until the reads are done */
    pthread_barrier_wait(&g_done);
    return NULL;
}

int main(void) {
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO | SA_NODEFER};
    if (sigaction(SIGSEGV, &sa, NULL) < 0)
        err(1, "sigaction");
    if (pthread_barrier_init(&g_done, NULL, MAPPINGS + 1))
        errx(1, "pthread_barrier_init");

    char* maps[MAPPINGS];
    pthread_t threads[MAPPINGS];
    for (int i = 0; i < MAPPINGS; i++) {
        maps[i] = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
        if (maps[i] == MAP_FAILED)
            err(1, "mmap");
        memset(maps[i], 'a' + i, MAPPING_SIZE);
        if (pthread_create(&threads[i], NULL, thread_fn, NULL))
            errx(1, "pthread_create");
    }

    volatile int reads = 0; /* volatile: counted after the volatile read, kept across the jump */
    for (int i = 0; i < MAPPINGS; i++) {
        volatile char* past_end = maps[i] + MAPPING_SIZE;
        if (sigsetjmp(g_jmp, 1) == 0) {
            char c = *past_end; /* the neighbour, or SIGSEGV */
            (void)c;
            reads++;
        }
    }
    printf("vma_neighbor: %d reads past the end, %d faults\n", reads, (int)g_faults);

    pthread_barrier_wait(&g_done);
    for (int i = 0; i < MAPPINGS; i++)
        pthread_join(threads[i], NULL);
    for (int i = 0; i < MAPPINGS; i++)
        munmap(maps[i], MAPPING_SIZE);
    puts("TEST OK");
    return 0;
}
