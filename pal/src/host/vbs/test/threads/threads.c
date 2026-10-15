/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Threads inside a VBS enclave (phase 3a): pthread_create/join, a mutex-protected counter and a
 * condition variable across several enclave threads; then how many threads can exist at once
 * (blocked threads are created until pthread_create fails), against the signed thread count.
 * Exit code 0 if every check passed.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define WORKERS 3
#define ROUNDS  200000
#define MAX_TRY 64

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static long g_counter;
static int g_started;
static int g_release;

static void* worker(void* arg) {
    pthread_mutex_lock(&g_lock);
    g_started++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter++;
        pthread_mutex_unlock(&g_lock);
    }
    return arg;
}

static void* sleeper(void* arg) {
    pthread_mutex_lock(&g_lock);
    g_started++;
    pthread_cond_broadcast(&g_cond);
    while (!g_release)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
    return arg;
}

static void* nop_thread(void* arg) {
    return arg;
}

int main(void) {
    int fails = 0;
    pthread_t t[MAX_TRY];
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1. workers, mutex, condvar, join */
    for (long i = 0; i < WORKERS; i++) {
        int r = pthread_create(&t[i], NULL, worker, (void*)i);
        if (r) {
            printf("pthread_create(worker %ld) failed: %s\n", i, strerror(r));
            return 1;
        }
    }
    pthread_mutex_lock(&g_lock);
    while (g_started < WORKERS)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
    for (long i = 0; i < WORKERS; i++) {
        void* ret;
        pthread_join(t[i], &ret);
        if ((long)ret != i)
            fails++;
    }
    printf("workers: %d threads x %d rounds, counter %ld (expected %ld): %s\n", WORKERS, ROUNDS,
           g_counter, (long)WORKERS * ROUNDS, g_counter == (long)WORKERS * ROUNDS ? "ok" : "WRONG");
    if (g_counter != (long)WORKERS * ROUNDS)
        fails++;

    /* 2. threads one after another (an enclave thread slot is reused each time) */
    for (long i = 0; i < 6; i++) {
        void* ret = NULL;
        int r = pthread_create(&t[0], NULL, nop_thread, (void*)(i + 100));
        if (r || pthread_join(t[0], &ret) || (long)ret != i + 100) {
            printf("sequential: thread %ld failed (%s)\n", i, r ? strerror(r) : "join");
            fails++;
            break;
        }
    }
    printf("sequential: 6 threads created and joined one after another\n");

    /* 3. how many threads at once: blocked sleepers until pthread_create fails */
    g_started = 0;
    int n = 0;
    for (; n < MAX_TRY; n++) {
        int r = pthread_create(&t[n], NULL, sleeper, NULL);
        if (r) {
            printf("limit: pthread_create of sleeper %d failed: %s\n", n + 1, strerror(r));
            break;
        }
        pthread_mutex_lock(&g_lock);
        while (g_started < n + 1)
            pthread_cond_wait(&g_cond, &g_lock);
        pthread_mutex_unlock(&g_lock);
    }
    printf("limit: %d sleeper threads ran at once besides main (and the LibOS's own threads)\n", n);
    pthread_mutex_lock(&g_lock);
    g_release = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < n; i++)
        pthread_join(t[i], NULL);
    printf("limit: all %d sleepers joined\n", n);
    if (n == 0 || n == MAX_TRY)
        fails++;

    printf("threads test: %s\n", fails ? "FAILED" : "passed");
    return fails;
}
