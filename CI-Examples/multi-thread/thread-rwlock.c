// rwlock_repro_musl.c
// Musl-friendly short rwlock repro
// Compile with musl-gcc:
//   unset CFLAGS CPPFLAGS LDFLAGS
//   musl-gcc -static -O2 -std=c11 -pthread -D_FORTIFY_SOURCE=0 \
//     rwlock_repro_musl.c -o rwlock_repro_musl
//
// Run:
//   ./rwlock_repro_musl [writers] [readers] [seconds]
// Defaults: writers=2 readers=6 seconds=3

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_rwlock_t rwlock;
static atomic_int keep_running = 1;

/* minimal integer parser (no libc strtol to avoid glibc-specific symbol issues) */
static long parse_long(const char* s, long def) {
    if (!s || !*s)
        return def;
    int neg = 0;
    if (*s == '+')
        s++;
    else if (*s == '-') {
        neg = 1;
        s++;
    }
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

/* debug macro prints epoch, kernel tid, pthread_self */
#define DBG(fmt, ...)                                                                             \
    fprintf(stderr, "[%ld][k:%d][pth:%lu] " fmt "\n", (long)time(NULL), (int)syscall(SYS_gettid), \
            (unsigned long)pthread_self(), ##__VA_ARGS__)

/* small interruptible sleep that checks keep_running frequently */
static void short_sleep_ms(int ms) {
    const int chunk = 20;  // ms slices
    int done        = 0;
    struct timespec ts;
    while (done < ms && atomic_load(&keep_running)) {
        int cur    = (ms - done) < chunk ? (ms - done) : chunk;
        ts.tv_sec  = cur / 1000;
        ts.tv_nsec = (cur % 1000) * 1000000;
        nanosleep(&ts, NULL);
        done += cur;
    }
}

/* writer: grab write lock, hold briefly, release; loop until keep_running==0 */
void* writer(void* arg) {
    int id = (int)(intptr_t)arg;
    while (atomic_load(&keep_running)) {
        int rc = pthread_rwlock_wrlock(&rwlock);
        if (rc != 0) {
            DBG("writer %d wrlock FAILED rc=%d (%s)", id, rc, strerror(rc));
            break;
        }
        DBG("writer %d holding write lock (short)", id);
        short_sleep_ms(120);  // hold ~120ms
        int rc2 = pthread_rwlock_unlock(&rwlock);
        if (rc2 != 0) {
            DBG("writer %d unlock FAILED rc=%d (%s)", id, rc2, strerror(rc2));
            break;
        } else {
            DBG("writer %d released write lock", id);
        }
        short_sleep_ms(30);
    }
    DBG("writer %d exiting", id);
    return NULL;
}

/* reader: try rdlock, short hold, unlock; loop until keep_running==0 */
void* reader(void* arg) {
    int id = (int)(intptr_t)arg;
    while (atomic_load(&keep_running)) {
        int rc = pthread_rwlock_rdlock(&rwlock);
        if (rc != 0) {
            DBG("reader %d rdlock FAILED rc=%d (%s)", id, rc, strerror(rc));
            break;
        }
        DBG("reader %d acquired read lock (short)", id);
        short_sleep_ms(40);  // hold ~40ms
        pthread_rwlock_unlock(&rwlock);
        short_sleep_ms(20);
    }
    DBG("reader %d exiting", id);
    return NULL;
}

int main(int argc, char** argv) {
    int nw                = 2;
    int nr                = 6;
    unsigned long seconds = 3;

    if (argc > 1)
        nw = (int)parse_long(argv[1], nw);
    if (argc > 2)
        nr = (int)parse_long(argv[2], nr);
    if (argc > 3)
        seconds = (unsigned long)parse_long(argv[3], seconds);

    DBG("main start writers=%d readers=%d seconds=%lu", nw, nr, seconds);

    int rc = pthread_rwlock_init(&rwlock, NULL);
    if (rc != 0) {
        fprintf(stderr, "rwlock_init rc=%d (%s)\n", rc, strerror(rc));
        return 1;
    }

    pthread_t wt[nw], rt[nr];
    for (int i = 0; i < nw; ++i) {
        int r = pthread_create(&wt[i], NULL, writer, (void*)(intptr_t)i);
        if (r != 0) {
            DBG("pthread_create writer %d failed rc=%d", i, r);
        } else {
            DBG("pthread_create writer %d OK", i);
        }
    }
    for (int i = 0; i < nr; ++i) {
        int r = pthread_create(&rt[i], NULL, reader, (void*)(intptr_t)i);
        if (r != 0) {
            DBG("pthread_create reader %d failed rc=%d", i, r);
        } else {
            DBG("pthread_create reader %d OK", i);
        }
    }

    /* run for configured time */
    for (unsigned long t = 0; t < seconds; ++t) sleep(1);

    DBG("main time elapsed, signaling workers to stop");
    atomic_store(&keep_running, 0);

    /* join threads (they will exit promptly since sleeps are interruptible via keep_running) */
    for (int i = 0; i < nw; ++i) {
        pthread_join(wt[i], NULL);
        DBG("joined writer %d", i);
    }
    for (int i = 0; i < nr; ++i) {
        pthread_join(rt[i], NULL);
        DBG("joined reader %d", i);
    }

    pthread_rwlock_destroy(&rwlock);
    DBG("main exit");
    return 0;
}
