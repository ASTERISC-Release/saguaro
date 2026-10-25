/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* madv_stress: stress of madvise(MADV_DONTNEED) for a demand-paged Saguaro VM process.
 *
 *   madv_stress burst [threads] [stack_mib] [iterations]
 *     each iteration starts `threads` threads with a stack of `stack_mib` MiB (pthread_attr_setstacksize),
 *     each touches a few pages of its stack, all wait on a barrier and exit at once (glibc then
 *     discards the unused part of every stack with madvise(MADV_DONTNEED)); the main thread waits
 *     1 s before joining, so every discard happens before any stack is unmapped.
 *
 *   madv_stress region [region_mib] [iterations] [allocators]
 *     maps an anonymous region, writes a pattern to every other page (the rest never touched), makes
 *     one written page in the middle PROT_NONE, discards the whole region, restores the protection and
 *     checks that every page reads zero, writes it all again; meanwhile `allocators` threads keep
 *     mapping, touching and unmapping 4 MiB chunks and one thread keeps writing a marker into the
 *     region during the discard (afterwards its pages hold 0 or the marker, nothing else).
 */
#define _GNU_SOURCE
#include <err.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static pthread_barrier_t g_barrier;
static size_t g_page;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void* burst_thread(void* arg) {
    volatile char buf[64 * 1024]; /* touches 16 pages of the stack */
    for (size_t i = 0; i < sizeof(buf); i += 4096)
        buf[i] = (char)(uintptr_t)arg;
    int r = pthread_barrier_wait(&g_barrier);
    if (r != 0 && r != PTHREAD_BARRIER_SERIAL_THREAD)
        errx(1, "pthread_barrier_wait: %d", r);
    return (void*)(uintptr_t)buf[4096];
}

static int burst(int threads, size_t stack_mib, int iterations) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_attr_setstacksize(&attr, stack_mib << 20))
        errx(1, "pthread_attr_setstacksize");
    pthread_t* t = calloc(threads, sizeof(*t));
    for (int it = 0; it < iterations; it++) {
        double t0 = now();
        if (pthread_barrier_init(&g_barrier, NULL, threads))
            errx(1, "pthread_barrier_init");
        for (int i = 0; i < threads; i++) {
            int r = pthread_create(&t[i], &attr, burst_thread, (void*)(uintptr_t)(i + 1));
            if (r)
                errx(1, "iteration %d: pthread_create %d: %s", it, i, strerror(r));
        }
        sleep(1);
        for (int i = 0; i < threads; i++)
            pthread_join(t[i], NULL);
        pthread_barrier_destroy(&g_barrier);
        printf("burst iteration %d: %d threads x %zu MiB ok (%.2f s)\n", it + 1, threads, stack_mib,
               now() - t0);
        fflush(stdout);
    }
    printf("BURST OK %d iterations\n", iterations);
    return 0;
}

static atomic_int g_stop;
static atomic_long g_alloc_rounds;
static char* g_region;
static size_t g_region_pages;
static atomic_int g_writer_go;
#define MARKER 0x5a5a5a5a

static void* allocator_thread(void* arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        size_t sz = 4 << 20;
        char* m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
            err(1, "allocator mmap");
        for (size_t o = 0; o < sz; o += g_page)
            m[o] = 1;
        for (size_t o = 0; o < sz; o += g_page)
            if (m[o] != 1)
                errx(1, "allocator: lost a write");
        munmap(m, sz);
        atomic_fetch_add(&g_alloc_rounds, 1);
    }
    return NULL;
}

/* writes the marker into the last quarter of the region (pages 3/4..end, every 4th page) while the
 * discard runs */
static void* writer_thread(void* arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        if (!atomic_load(&g_writer_go)) {
            sched_yield();
            continue;
        }
        for (size_t p = g_region_pages * 3 / 4; p < g_region_pages; p += 4)
            *(volatile int*)(g_region + p * g_page + 8) = MARKER;
    }
    return NULL;
}

static int region(size_t mib, int iterations, int allocators) {
    size_t size = mib << 20;
    g_region_pages = size / g_page;
    g_region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_region == MAP_FAILED)
        err(1, "mmap region");
    pthread_t at[64], wt;
    if (allocators > 64)
        allocators = 64;
    for (int i = 0; i < allocators; i++)
        pthread_create(&at[i], NULL, allocator_thread, NULL);
    pthread_create(&wt, NULL, writer_thread, NULL);
    size_t mid = g_region_pages / 2;
    if (mid % 2)
        mid++;
    for (int it = 0; it < iterations; it++) {
        double t0 = now();
        /* even pages written, odd pages of the first 3/4 never touched in this iteration */
        for (size_t p = 0; p < g_region_pages * 3 / 4; p += 2)
            *(int*)(g_region + p * g_page) = (int)(p ^ it) | 1;
        if (mprotect(g_region + mid * g_page, g_page, PROT_NONE))
            err(1, "mprotect PROT_NONE");
        atomic_store(&g_writer_go, 1);
        double tm = now();
        if (madvise(g_region, size, MADV_DONTNEED))
            err(1, "madvise");
        tm = now() - tm;
        atomic_store(&g_writer_go, 0);
        usleep(1000); /* let the writer finish its pass */
        if (mprotect(g_region + mid * g_page, g_page, PROT_READ | PROT_WRITE))
            err(1, "mprotect RW");
        size_t bad = 0, markers = 0;
        for (size_t p = 0; p < g_region_pages * 3 / 4; p++) {
            const int* w = (const int*)(g_region + p * g_page);
            for (size_t k = 0; k < g_page / sizeof(int); k += 97)
                if (w[k]) {
                    if (bad++ < 5)
                        fprintf(stderr, "iteration %d: page %zu word %zu = 0x%x (expected 0)%s\n", it,
                                p, k, w[k], p == mid ? " [the PROT_NONE page]" : "");
                }
        }
        for (size_t p = g_region_pages * 3 / 4; p < g_region_pages; p++) {
            int v = *(const int*)(g_region + p * g_page + 8);
            if (v == MARKER)
                markers++;
            else if (v != 0 && bad++ < 5)
                fprintf(stderr, "iteration %d: writer page %zu = 0x%x (expected 0 or marker)\n", it,
                        p, v);
        }
        if (bad)
            errx(1, "iteration %d: %zu bad words after MADV_DONTNEED", it, bad);
        /* touch everything once more (uses the whole region's frames) and clear the markers */
        for (size_t p = 0; p < g_region_pages; p++)
            memset(g_region + p * g_page, 0, 16);
        printf("region iteration %d: %zu MiB ok, %zu marker pages kept, %ld allocator rounds (%.2f s, madvise %.3f s)\n",
               it + 1, mib, markers, atomic_load(&g_alloc_rounds), now() - t0, tm);
        fflush(stdout);
    }
    atomic_store(&g_stop, 1);
    for (int i = 0; i < allocators; i++)
        pthread_join(at[i], NULL);
    pthread_join(wt, NULL);
    munmap(g_region, size);
    printf("REGION OK %d iterations\n", iterations);
    return 0;
}

int main(int argc, char** argv) {
    g_page = getpagesize();
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "burst"))
        return burst(argc > 2 ? atoi(argv[2]) : 48, argc > 3 ? strtoul(argv[3], NULL, 0) : 32,
                     argc > 4 ? atoi(argv[4]) : 20);
    if (argc >= 2 && !strcmp(argv[1], "region"))
        return region(argc > 2 ? strtoul(argv[2], NULL, 0) : 512, argc > 3 ? atoi(argv[3]) : 20,
                      argc > 4 ? atoi(argv[4]) : 4);
    fprintf(stderr, "usage: %s burst [threads] [stack_mib] [iterations] | region [mib] [iterations] [allocators]\n",
            argv[0]);
    return 2;
}
