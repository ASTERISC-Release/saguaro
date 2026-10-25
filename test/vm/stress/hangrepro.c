/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Minimal reproducers for the VM PAL hangs.
 *   hangrepro join ITER NTHR [WORK]  ITER rounds: create NTHR threads, each does WORK (f = futex
 *                                    wait with timeout, s = nanosleep, m = mmap/munmap, default
 *                                    "fsm") a few times, returns; main joins them all.
 *   hangrepro ring SECONDS           futex ring of 6 threads (token passing, random sleeps), timed
 *                                    futex waits as in idlebench stress
 *   hangrepro twait SECONDS NTHR     NTHR threads in a loop of FUTEX_WAIT with short timeouts that
 *                                    nobody wakes: every wait must end by its timeout
 *   hangrepro spawn NCREATORS NCHILD N creator threads each create NCHILD threads at once (fresh
 *                                    PAL stacks, allocated concurrently); children mmap and touch a
 *                                    page and wait until all exist; then everything is joined
 *   hangrepro ptmap ITER NTHR        ITER rounds: NTHR threads released together each mmap one page
 *                                    (MAP_FIXED_NOREPLACE) in the same fresh 2-MiB region (a new
 *                                    page table, and every 64 rounds a new 1-GiB one) and write it
 * A watchdog thread prints HANG/STALL with per-thread state when no progress for 5 s (and keeps
 * waiting, so the guest can be inspected), unless HANGREPRO_EXIT=1 (then it exits with code 2).
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define MAXT 64
static atomic_ullong progress;
static atomic_int tstate[MAXT]; /* 0 not started, 1 running, 2 in futex wait, 3 sleeping, 4 mmap, 9 done */
static atomic_ullong tcount[MAXT];
static int nthr;
static atomic_int phase; /* 1 = joining */
static atomic_int iter_now;
static const char* work = "fsm";

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static unsigned rnd(unsigned* s) {
    *s = *s * 1103515245 + 12345;
    return *s >> 8;
}
static void sleep_us(long us) {
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}
static long fxw(atomic_int* a, int v, long ns) {
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    return syscall(SYS_futex, a, FUTEX_WAIT, v, ns ? &ts : NULL, NULL, 0);
}
static long fxwake(atomic_int* a, int n) {
    return syscall(SYS_futex, a, FUTEX_WAKE, n, NULL, NULL, 0);
}

static void* watchdog(void* x) {
    (void)x;
    unsigned long long last = atomic_load(&progress);
    int quiet = 0, reported = 0;
    for (;;) {
        sleep(1);
        unsigned long long p = atomic_load(&progress);
        if (p != last) {
            last = p;
            quiet = 0;
            reported = 0;
            continue;
        }
        if (++quiet >= 5 && !reported) {
            reported = 1;
            printf("HANG: no progress for %d s, iter=%d phase=%d states:", quiet,
                   atomic_load(&iter_now), atomic_load(&phase));
            for (int i = 0; i < nthr; i++)
                printf(" %d:%d/%llu", i, atomic_load(&tstate[i]), atomic_load(&tcount[i]));
            printf("\n");
            fflush(stdout);
            if (getenv("HANGREPRO_EXIT") && atoi(getenv("HANGREPRO_EXIT")))
                _exit(2);
        }
    }
    return NULL;
}

/* ---- join ---- */
static atomic_int shared_word;
static void* join_worker(void* x) {
    int me = (int)(intptr_t)x;
    unsigned s = 1234 + me * 77 + atomic_load(&iter_now) * 13;
    atomic_store(&tstate[me], 1);
    for (int k = 0; k < 4; k++) {
        for (const char* w = work; *w; w++) {
            if (*w == 'f') {
                atomic_store(&tstate[me], 2);
                fxw(&shared_word, 0, (1 + rnd(&s) % 500) * 1000); /* 1..500 us, nobody wakes */
            } else if (*w == 's') {
                atomic_store(&tstate[me], 3);
                sleep_us(rnd(&s) % 300);
            } else if (*w == 'm') {
                atomic_store(&tstate[me], 4);
                size_t pages = 1 + rnd(&s) % 16;
                char* p = mmap(NULL, pages * 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED) {
                    printf("mmap failed\n");
                    exit(3);
                }
                for (size_t i = 0; i < pages; i++)
                    p[i * 4096] = 1;
                munmap(p, pages * 4096);
            }
            atomic_store(&tstate[me], 1);
            atomic_fetch_add(&tcount[me], 1);
            atomic_fetch_add(&progress, 1);
        }
    }
    atomic_store(&tstate[me], 9);
    return NULL;
}
static int do_join(int iters, int n) {
    nthr = n;
    pthread_t th[MAXT];
    uint64_t t0 = now_ns();
    for (int it = 0; it < iters; it++) {
        atomic_store(&iter_now, it);
        atomic_store(&phase, 0);
        for (int i = 0; i < n; i++) {
            atomic_store(&tstate[i], 0);
            if (pthread_create(&th[i], NULL, join_worker, (void*)(intptr_t)i)) {
                printf("pthread_create failed\n");
                return 3;
            }
        }
        atomic_store(&phase, 1);
        for (int i = 0; i < n; i++) {
            pthread_join(th[i], NULL);
            atomic_fetch_add(&progress, 1);
        }
        if ((it + 1) % 100 == 0)
            printf("join: %d iterations, %.1f s\n", it + 1, (now_ns() - t0) / 1e9);
    }
    printf("JOIN OK %d x %d\n", iters, n);
    return 0;
}

/* ---- ring ---- */
#define RING 6
static atomic_int ring_word[RING];
static atomic_int stop;
static atomic_ullong cnt_ring;
static void spin_ns(uint64_t ns) {
    uint64_t end = now_ns() + ns;
    while (now_ns() < end)
        __asm__ volatile("pause");
}
static void* ring_worker(void* x) {
    int me = (int)(intptr_t)x, next = (me + 1) % RING;
    unsigned s = 1234 + me;
    while (!atomic_load(&stop)) {
        while (atomic_load_explicit(&ring_word[me], memory_order_acquire) == 0) {
            if (atomic_load(&stop))
                goto out;
            atomic_store(&tstate[me], 2);
            fxw(&ring_word[me], 0, 200 * 1000 * 1000);
            atomic_store(&tstate[me], 1);
            atomic_fetch_add(&tcount[me], 1);
        }
        atomic_store(&ring_word[me], 0);
        atomic_fetch_add(&cnt_ring, 1);
        atomic_fetch_add(&progress, 1);
        unsigned r = rnd(&s) % 100;
        if (r < 30) {
            atomic_store(&tstate[me], 3);
            sleep_us(rnd(&s) % 2000);
            atomic_store(&tstate[me], 1);
        } else if (r < 60) {
            spin_ns((rnd(&s) % 200) * 1000);
        }
        atomic_store_explicit(&ring_word[next], 1, memory_order_release);
        fxwake(&ring_word[next], 1);
    }
out:
    atomic_store(&tstate[me], 9);
    return NULL;
}
static int do_ring(int seconds) {
    nthr = RING;
    pthread_t th[RING];
    for (int i = 0; i < RING; i++)
        pthread_create(&th[i], NULL, ring_worker, (void*)(intptr_t)i);
    atomic_store_explicit(&ring_word[0], 1, memory_order_release);
    fxwake(&ring_word[0], 1);
    unsigned long long last = 0;
    int stuck = 0, rc = 0;
    for (int sec = 1; sec <= seconds; sec++) {
        sleep(1);
        unsigned long long c = atomic_load(&cnt_ring);
        stuck = c == last ? stuck + 1 : 0;
        last = c;
        if (stuck >= 5) {
            printf("STALL ring at %d s (count %llu) states:", sec, c);
            for (int i = 0; i < RING; i++)
                printf(" %d:%d/%llu/w%d", i, atomic_load(&tstate[i]), atomic_load(&tcount[i]),
                       atomic_load(&ring_word[i]));
            printf("\n");
            rc = 2;
            if (getenv("HANGREPRO_EXIT") && atoi(getenv("HANGREPRO_EXIT")))
                _exit(2);
            break;
        }
        if (sec % 10 == 0)
            printf("t=%d ring=%llu\n", sec, c);
    }
    atomic_store(&stop, 1);
    atomic_store(&phase, 1);
    for (int i = 0; i < RING; i++) {
        atomic_store(&ring_word[i], 1);
        fxwake(&ring_word[i], 1);
    }
    for (int i = 0; i < RING; i++) {
        pthread_join(th[i], NULL);
        atomic_fetch_add(&progress, 1);
    }
    printf(rc ? "RING FAILED\n" : "RING OK\n");
    return rc;
}

/* ---- timed waits nobody wakes ---- */
static atomic_int never;
static void* twait_worker(void* x) {
    int me = (int)(intptr_t)x;
    unsigned s = 4321 + me;
    while (!atomic_load(&stop)) {
        atomic_store(&tstate[me], 2);
        fxw(&never, 0, (1 + rnd(&s) % 2000) * 1000);
        atomic_store(&tstate[me], 1);
        atomic_fetch_add(&tcount[me], 1);
        atomic_fetch_add(&progress, 1);
    }
    atomic_store(&tstate[me], 9);
    return NULL;
}
static int do_twait(int seconds, int n) {
    nthr = n;
    pthread_t th[MAXT];
    for (int i = 0; i < n; i++)
        pthread_create(&th[i], NULL, twait_worker, (void*)(intptr_t)i);
    unsigned long long last[MAXT] = {0};
    int stuck[MAXT] = {0}, rc = 0;
    for (int sec = 1; sec <= seconds && !rc; sec++) {
        sleep(1);
        for (int i = 0; i < n; i++) {
            unsigned long long c = atomic_load(&tcount[i]);
            stuck[i] = c == last[i] ? stuck[i] + 1 : 0;
            last[i] = c;
            if (stuck[i] >= 5) {
                printf("STALL twait thread %d (state %d, count %llu) at %d s\n", i,
                       atomic_load(&tstate[i]), c, sec);
                rc = 2;
                if (getenv("HANGREPRO_EXIT") && atoi(getenv("HANGREPRO_EXIT")))
                    _exit(2);
            }
        }
        if (sec % 10 == 0) {
            unsigned long long tot = 0;
            for (int i = 0; i < n; i++)
                tot += atomic_load(&tcount[i]);
            printf("t=%d waits=%llu\n", sec, tot);
        }
    }
    atomic_store(&stop, 1);
    atomic_store(&phase, 1);
    for (int i = 0; i < n; i++) {
        pthread_join(th[i], NULL);
        atomic_fetch_add(&progress, 1);
    }
    printf(rc ? "TWAIT FAILED\n" : "TWAIT OK\n");
    return rc;
}

/* ---- concurrent mmap into the same fresh page table ---- */
static atomic_int pt_go, pt_done;
static uintptr_t pt_region(int it) {
    return 0x4000000000ull + (uintptr_t)(it / 64) * 0x40000000ull + (uintptr_t)(it % 64) * 0x200000ull * 4;
}
static void* ptmap_worker(void* x) {
    int me = (int)(intptr_t)x;
    for (int it = 0;; it++) {
        int g;
        while ((g = atomic_load(&pt_go)) <= it)
            fxw(&pt_go, g, 0);
        if (atomic_load(&stop))
            return NULL;
        char* want = (char*)(pt_region(it) + (uintptr_t)me * 4096);
        char* p = mmap(want, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != want) {
            printf("ptmap: mmap at %p failed (%p, errno %d) round %d\n", want, p, errno, it);
            exit(3);
        }
        p[0] = 1;
        p[100] = (char)me;
        if (atomic_fetch_add(&pt_done, 1) + 1 == nthr)
            fxwake(&pt_done, 1);
        atomic_fetch_add(&progress, 1);
    }
}
static int do_ptmap(int iters, int n) {
    nthr = n;
    pthread_t th[MAXT];
    for (int i = 0; i < n; i++)
        pthread_create(&th[i], NULL, ptmap_worker, (void*)(intptr_t)i);
    for (int it = 0; it < iters; it++) {
        atomic_store(&pt_done, 0);
        atomic_store(&pt_go, it + 1);
        fxwake(&pt_go, n);
        int d;
        while ((d = atomic_load(&pt_done)) < n)
            fxw(&pt_done, d, 0);
        for (int i = 0; i < n; i++) {
            char* p = (char*)(pt_region(it) + (uintptr_t)i * 4096);
            if (p[100] != (char)i) {
                printf("ptmap: lost page %d of round %d\n", i, it);
                return 2;
            }
            munmap(p, 4096);
        }
        if ((it + 1) % 100 == 0)
            printf("ptmap: %d rounds\n", it + 1);
    }
    atomic_store(&stop, 1);
    atomic_store(&pt_go, iters + 1);
    fxwake(&pt_go, n);
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    printf("PTMAP OK %d x %d\n", iters, n);
    return 0;
}

/* ---- concurrent thread creation ---- */
static atomic_int sp_alive, sp_release;
static int sp_total;
static void* sp_child(void* x) {
    (void)x;
    char* p = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        exit(3);
    p[0] = p[4096] = p[8192] = 1;
    if (atomic_fetch_add(&sp_alive, 1) + 1 == sp_total)
        fxwake(&sp_alive, 1);
    while (!atomic_load(&sp_release))
        fxw(&sp_release, 0, 0);
    munmap(p, 3 * 4096);
    return NULL;
}
static atomic_int sp_go;
static void* sp_creator(void* x) {
    int nchild = (int)(intptr_t)x;
    pthread_t th[256];
    while (!atomic_load(&sp_go))
        fxw(&sp_go, 0, 0);
    for (int i = 0; i < nchild; i++)
        if (pthread_create(&th[i], NULL, sp_child, NULL)) {
            printf("pthread_create failed\n");
            exit(3);
        }
    for (int i = 0; i < nchild; i++)
        pthread_join(th[i], NULL);
    return NULL;
}
static int do_spawn(int ncreators, int nchild) {
    pthread_t cr[MAXT];
    sp_total = ncreators * nchild;
    for (int i = 0; i < ncreators; i++)
        pthread_create(&cr[i], NULL, sp_creator, (void*)(intptr_t)nchild);
    atomic_store(&sp_go, 1);
    fxwake(&sp_go, ncreators);
    int a;
    while ((a = atomic_load(&sp_alive)) < sp_total)
        fxw(&sp_alive, a, 0);
    atomic_store(&sp_release, 1);
    fxwake(&sp_release, sp_total);
    for (int i = 0; i < ncreators; i++)
        pthread_join(cr[i], NULL);
    printf("SPAWN OK %d x %d\n", ncreators, nchild);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: hangrepro join ITER NTHR [WORK] | ring SECONDS | twait SECONDS NTHR\n");
        return 1;
    }
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    int rc = 1;
    if (!strcmp(argv[1], "join")) {
        if (argc > 4)
            work = argv[4];
        rc = do_join(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 8);
    } else if (!strcmp(argv[1], "ring")) {
        rc = do_ring(atoi(argv[2]));
    } else if (!strcmp(argv[1], "spawn")) {
        rc = do_spawn(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 8);
    } else if (!strcmp(argv[1], "ptmap")) {
        rc = do_ptmap(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 8);
    } else if (!strcmp(argv[1], "twait")) {
        rc = do_twait(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 8);
    }
    printf("EXIT rc=%d\n", rc);
    return rc;
}
