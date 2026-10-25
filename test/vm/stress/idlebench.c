/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* idlebench: idle-vCPU micro-benchmarks and stress test (guest and native).
 *   idlebench idle SECONDS           sleep (the guest is idle; measure host CPU from outside)
 *   idlebench pingpong [N]           futex ping-pong between two threads, us per round trip
 *   idlebench wakelat [K]            wake-up latency of a thread blocked in FUTEX_WAIT after the
 *                                    waker spun GAP us (GAP = 0, 20, 50, 100, 200, 1000, 10000)
 *   idlebench stress SECONDS         futex ring + random sleeps, mmap/munmap churn (TLB
 *                                    shootdowns), short-lived threads, a pinned waiter; prints
 *                                    progress every 10 s and STALL if a worker stops progressing
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static long fx(atomic_int* a, int op, int v) {
    return syscall(SYS_futex, a, op, v, NULL, NULL, 0);
}
static void spin_ns(uint64_t ns) {
    uint64_t end = now_ns() + ns;
    while (now_ns() < end)
        __asm__ volatile("pause");
}
static void sleep_us(long us) {
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}

/* ---- ping-pong (the probe of saguaro-perf) ---- */
static atomic_int turn;
static int N = 200000;
static void* pp_worker(void* x) {
    (void)x;
    for (int i = 0; i < N; i++) {
        while (atomic_load_explicit(&turn, memory_order_acquire) != 1)
            fx(&turn, FUTEX_WAIT, 0);
        atomic_store_explicit(&turn, 0, memory_order_release);
        fx(&turn, FUTEX_WAKE, 1);
    }
    return NULL;
}
static void pingpong(void) {
    atomic_store(&turn, 0);
    pthread_t t;
    pthread_create(&t, NULL, pp_worker, NULL);
    double t0 = now();
    for (int i = 0; i < N; i++) {
        atomic_store_explicit(&turn, 1, memory_order_release);
        fx(&turn, FUTEX_WAKE, 1);
        while (atomic_load_explicit(&turn, memory_order_acquire) != 0)
            fx(&turn, FUTEX_WAIT, 1);
    }
    double dt = now() - t0;
    pthread_join(t, NULL);
    printf("pingpong futex %.2f us/roundtrip\n", dt / N * 1e6);
}

/* ---- wake-up latency ---- */
static atomic_int wl_word;      /* 0: waiter must wait, 1: go */
static atomic_int wl_ready;     /* waiter is about to wait */
static atomic_ullong wl_t1;
static atomic_int wl_stop;
static void* wl_waiter(void* x) {
    (void)x;
    while (!atomic_load(&wl_stop)) {
        atomic_store(&wl_ready, 1);
        while (atomic_load_explicit(&wl_word, memory_order_acquire) == 0)
            fx(&wl_word, FUTEX_WAIT, 0);
        atomic_store(&wl_t1, now_ns());
        atomic_store(&wl_word, 0);
        atomic_store(&wl_ready, 0);
        /* hand back: the waker polls wl_t1 */
        while (atomic_load(&wl_t1) && !atomic_load(&wl_stop))
            __asm__ volatile("pause");
    }
    return NULL;
}
static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}
static void wakelat(int K) {
    static const int gaps[] = {0, 20, 50, 100, 200, 1000, 10000};
    pthread_t t;
    pthread_create(&t, NULL, wl_waiter, NULL);
    uint64_t* lat = malloc(sizeof(uint64_t) * K);
    for (size_t g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
        int k = gaps[g] >= 10000 ? K / 10 : K;
        for (int i = 0; i < k; i++) {
            while (!atomic_load(&wl_ready))
                __asm__ volatile("pause");
            /* the waiter is in (or about to enter) FUTEX_WAIT; give it 5 us to block, then GAP */
            spin_ns(5000 + (uint64_t)gaps[g] * 1000);
            uint64_t t0 = now_ns();
            atomic_store_explicit(&wl_word, 1, memory_order_release);
            fx(&wl_word, FUTEX_WAKE, 1);
            uint64_t t1;
            while (!(t1 = atomic_load(&wl_t1)))
                __asm__ volatile("pause");
            lat[i] = t1 - t0;
            atomic_store(&wl_t1, 0);
        }
        qsort(lat, k, sizeof(uint64_t), cmp_u64);
        double sum = 0;
        for (int i = 0; i < k; i++)
            sum += lat[i];
        printf("wakelat gap=%5dus n=%d mean=%.2f p50=%.2f p90=%.2f p99=%.2f max=%.2f us\n", gaps[g],
               k, sum / k / 1e3, lat[k / 2] / 1e3, lat[k * 9 / 10] / 1e3, lat[k * 99 / 100] / 1e3,
               lat[k - 1] / 1e3);
        fflush(stdout);
    }
    atomic_store(&wl_stop, 1);
    atomic_store(&wl_word, 1);
    fx(&wl_word, FUTEX_WAKE, 1);
    pthread_join(t, NULL);
}

/* ---- stress ---- */
#define RING 6
static atomic_int ring_word[RING];
static atomic_ullong cnt_ring, cnt_mmap, cnt_threads, cnt_pinned, max_handoff_ns, max_pinned_ns;
static atomic_ullong ring_t0;
static atomic_int stop;
static unsigned rnd(unsigned* s) {
    *s = *s * 1103515245 + 12345;
    return *s >> 8;
}
static void* ring_worker(void* x) {
    int me = (int)(intptr_t)x, next = (me + 1) % RING;
    unsigned s = 1234 + me;
    while (!atomic_load(&stop)) {
        while (atomic_load_explicit(&ring_word[me], memory_order_acquire) == 0) {
            if (atomic_load(&stop))
                return NULL;
            struct timespec ts = {0, 200 * 1000 * 1000};
            syscall(SYS_futex, &ring_word[me], FUTEX_WAIT, 0, &ts, NULL, 0);
        }
        uint64_t t = now_ns(), t0 = atomic_load(&ring_t0);
        if (t0 && t - t0 > atomic_load(&max_handoff_ns))
            atomic_store(&max_handoff_ns, t - t0);
        atomic_store(&ring_word[me], 0);
        atomic_fetch_add(&cnt_ring, 1);
        unsigned r = rnd(&s) % 100;
        if (r < 30)
            sleep_us(rnd(&s) % 2000);          /* let CPUs go idle and halt */
        else if (r < 60)
            spin_ns((rnd(&s) % 200) * 1000);   /* short busy work */
        atomic_store(&ring_t0, now_ns());
        atomic_store_explicit(&ring_word[next], 1, memory_order_release);
        fx(&ring_word[next], FUTEX_WAKE, 1);
    }
    return NULL;
}
static void* mmap_worker(void* x) {
    unsigned s = 999 + (int)(intptr_t)x;
    while (!atomic_load(&stop)) {
        size_t pages = 1 + rnd(&s) % 64;
        char* p = mmap(NULL, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                       0);
        if (p == MAP_FAILED) {
            printf("mmap failed\n");
            exit(3);
        }
        for (size_t i = 0; i < pages; i++)
            p[i * 4096] = (char)i;
        if (rnd(&s) % 4 == 0)
            mprotect(p, pages * 4096, PROT_READ);
        munmap(p, pages * 4096);
        atomic_fetch_add(&cnt_mmap, 1);
        if (rnd(&s) % 8 == 0)
            sleep_us(rnd(&s) % 500);
    }
    return NULL;
}
static void* short_thread(void* x) {
    (void)x;
    atomic_fetch_add(&cnt_threads, 1);
    return NULL;
}
static void* spawner(void* x) {
    unsigned s = 77 + (int)(intptr_t)x;
    while (!atomic_load(&stop)) {
        pthread_t t[4];
        int n = 1 + rnd(&s) % 4;
        for (int i = 0; i < n; i++)
            if (pthread_create(&t[i], NULL, short_thread, NULL)) {
                printf("pthread_create failed\n");
                exit(3);
            }
        for (int i = 0; i < n; i++)
            pthread_join(t[i], NULL);
        sleep_us(rnd(&s) % 1000);
    }
    return NULL;
}
static atomic_int pin_word;
static atomic_ullong pin_t0;
static void* pinned_waiter(void* x) {
    long ncpu = (long)(intptr_t)x;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(ncpu - 1, &set);
    if (sched_setaffinity(0, sizeof(set), &set))
        printf("sched_setaffinity failed (ignored)\n");
    while (!atomic_load(&stop)) {
        while (atomic_load_explicit(&pin_word, memory_order_acquire) == 0) {
            if (atomic_load(&stop))
                return NULL;
            struct timespec ts = {0, 200 * 1000 * 1000};
            syscall(SYS_futex, &pin_word, FUTEX_WAIT, 0, &ts, NULL, 0);
        }
        uint64_t d = now_ns() - atomic_load(&pin_t0);
        if (d > atomic_load(&max_pinned_ns))
            atomic_store(&max_pinned_ns, d);
        atomic_store(&pin_word, 0);
        atomic_fetch_add(&cnt_pinned, 1);
    }
    return NULL;
}
static void* pinned_waker(void* x) {
    (void)x;
    unsigned s = 4242;
    while (!atomic_load(&stop)) {
        sleep_us(500 + rnd(&s) % 3000);
        if (atomic_load(&pin_word))
            continue;
        atomic_store(&pin_t0, now_ns());
        atomic_store_explicit(&pin_word, 1, memory_order_release);
        fx(&pin_word, FUTEX_WAKE, 1);
    }
    return NULL;
}
static int stress(int seconds, const char* mode) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    printf("stress: %d s, %ld CPUs online, workers %s\n", seconds, ncpu, mode);
    int want_ring = !!strchr(mode, 'r'), want_mmap = !!strchr(mode, 'm');
    int want_thr = !!strchr(mode, 't'), want_pin = !!strchr(mode, 'p');
    pthread_t th[RING + 8];
    int n = 0;
    for (int i = 0; want_ring && i < RING; i++)
        pthread_create(&th[n++], NULL, ring_worker, (void*)(intptr_t)i);
    for (int i = 0; want_mmap && i < 2; i++)
        pthread_create(&th[n++], NULL, mmap_worker, (void*)(intptr_t)i);
    for (int i = 0; want_thr && i < 2; i++)
        pthread_create(&th[n++], NULL, spawner, (void*)(intptr_t)i);
    if (want_pin) {
        pthread_create(&th[n++], NULL, pinned_waiter, (void*)(intptr_t)ncpu);
        pthread_create(&th[n++], NULL, pinned_waker, NULL);
    }
    atomic_store(&ring_t0, now_ns());
    atomic_store_explicit(&ring_word[0], 1, memory_order_release);
    fx(&ring_word[0], FUTEX_WAKE, 1);

    unsigned long long last[4] = {0}, stuck[4] = {0};
    int rc = 0;
    for (int sec = 1; sec <= seconds; sec++) {
        sleep(1);
        unsigned long long c[4] = {atomic_load(&cnt_ring), atomic_load(&cnt_mmap),
                                   atomic_load(&cnt_threads), atomic_load(&cnt_pinned)};
        int want[4] = {want_ring, want_mmap, want_thr, want_pin};
        for (int i = 0; i < 4; i++) {
            if (!want[i])
                continue;
            stuck[i] = c[i] == last[i] ? stuck[i] + 1 : 0;
            last[i] = c[i];
            if (stuck[i] >= 5) {
                printf("STALL worker-kind %d (no progress for %llu s) at %d s\n", i, stuck[i], sec);
                rc = 2;
            }
        }
        if (sec % 10 == 0 || rc) {
            printf("t=%d ring=%llu mmap=%llu threads=%llu pinned=%llu max_handoff=%.2fms "
                   "max_pinned=%.2fms\n", sec, c[0], c[1], c[2], c[3],
                   atomic_load(&max_handoff_ns) / 1e6, atomic_load(&max_pinned_ns) / 1e6);
            fflush(stdout);
        }
        if (rc)
            break;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < RING; i++) {
        atomic_store(&ring_word[i], 1);
        fx(&ring_word[i], FUTEX_WAKE, 1);
    }
    atomic_store(&pin_word, 1);
    fx(&pin_word, FUTEX_WAKE, 1);
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    printf(rc ? "STRESS FAILED\n" : "STRESS OK\n");
    return rc;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: idlebench idle|pingpong|wakelat|stress [arg]\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(argv[1], "idle")) {
        int s = argc > 2 ? atoi(argv[2]) : 30;
        printf("idle: sleeping %d s\n", s);
        sleep(s);
        printf("idle: done\n");
    } else if (!strcmp(argv[1], "pingpong")) {
        if (argc > 2)
            N = atoi(argv[2]);
        pingpong();
    } else if (!strcmp(argv[1], "wakelat")) {
        wakelat(argc > 2 ? atoi(argv[2]) : 2000);
    } else if (!strcmp(argv[1], "stress")) {
        return stress(argc > 2 ? atoi(argv[2]) : 60, argc > 3 ? argv[3] : "rmtp");
    } else if (!strcmp(argv[1], "all")) {
        pingpong();
        wakelat(argc > 2 ? atoi(argv[2]) : 2000);
    }
    return 0;
}
