// thread_stress_debug_grab_backtrace.c
// Compile: gcc -O2 -std=c11 -pthread -rdynamic -g -o thread_stress_debug_grab_backtrace thread_stress_debug_grab_backtrace.c
// -rdynamic helps backtrace() to resolve symbols. -g gives line numbers if available.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <execinfo.h>
#include <sys/syscall.h>

#define N_WORKERS 10
#define DEFAULT_SECONDS 10

static atomic_long global_counter = 0;
static pthread_mutex_t try_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rwlock;
static sem_t sem;
static atomic_long malloc_frees = 0;
static atomic_long failed_ops = 0;
static atomic_long lock_contentions = 0;
static volatile int keep_running = 1;

/* thread-local write-hold counter */
static __thread int write_hold_count = 0;

/* debug to stderr with epoch and kernel tid + pthread_self */
#define DBG(fmt, ...) \
    fprintf(stderr, "[%ld][k_tid:%d][pth:%lu] " fmt "\n", \
            (long)time(NULL), (int)syscall(SYS_gettid), (unsigned long)pthread_self(), ##__VA_ARGS__)

/* print backtrace to stderr */
static void print_backtrace(const char *label) {
    void *buf[32];
    int n = backtrace(buf, sizeof(buf)/sizeof(buf[0]));
    fprintf(stderr, "---- BACKTRACE (%s) ----\n", label);
    backtrace_symbols_fd(buf, n, fileno(stderr));
    fprintf(stderr, "---- end backtrace ----\n");
}

static void random_pause(void) {
    struct timespec t = {0, (rand() & 0x3FF) * 1000}; // 0..~1ms
    nanosleep(&t, NULL);
}

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    DBG("worker %d start", id);
    unsigned long iters = 0;
    while (keep_running) {
        /* mostly readers */
        if ((rand() & 0xFF) < 230) {
            /* diagnostic check before performing rdlock */
            if (write_hold_count > 0) {
                DBG("WARNING worker %d has write_hold_count=%d BEFORE rdlock", id, write_hold_count);
            }

            int rc = pthread_rwlock_rdlock(&rwlock);
            if (rc == 0) {
                (void)atomic_load(&global_counter);
                pthread_rwlock_unlock(&rwlock);
            } else {
                /* print detailed diagnostic and backtrace */
                DBG("worker %d rdlock FAILED rc=%d (%s)", id, rc, strerror(rc));
                if (rc == EDEADLK) {
                    DBG("  -> EDEADLK: thread appears to hold write lock; dumping backtrace");
                    print_backtrace("rdlock EDEADLK");
                } else if (rc == EINVAL) {
                    DBG("  -> EINVAL: rwlock invalid/uninitialized");
                    print_backtrace("rdlock EINVAL");
                } else {
                    DBG("  -> rc=%d (see man pthread_rwlock_rdlock)", rc);
                    print_backtrace("rdlock OTHER");
                }
                atomic_fetch_add(&failed_ops, 1);
            }
        } else {
            /* writer */
            int rc = pthread_rwlock_wrlock(&rwlock);
            if (rc == 0) {
                write_hold_count++;
                long before = atomic_load(&global_counter);
                atomic_fetch_add(&global_counter, 1);
                long after = atomic_load(&global_counter);
                DBG("worker %d wrlock acquired, %ld -> %ld", id, before, after);
                random_pause();
                int rc2 = pthread_rwlock_unlock(&rwlock);
                if (rc2 != 0) {
                    DBG("worker %d wrlock UNLOCK FAILED rc=%d (%s)", id, rc2, strerror(rc2));
                    print_backtrace("unlock failed");
                } else {
                    write_hold_count--;
                    DBG("worker %d wrlock released", id);
                }
            } else {
                DBG("worker %d wrlock FAILED rc=%d (%s)", id, rc, strerror(rc));
                atomic_fetch_add(&failed_ops, 1);
            }
        }

        /* trylock path occasionally */
        if ((rand() & 0x3FF) == 0) {
            if (pthread_mutex_trylock(&try_mutex) == 0) {
                DBG("worker %d trylock succeeded", id);
                pthread_mutex_unlock(&try_mutex);
            } else {
                atomic_fetch_add(&lock_contentions, 1);
                DBG("worker %d trylock contention", id);
            }
        }

        /* malloc/free churn occasionally */
        if ((rand() & 0xFF) == 0) {
            size_t s = (rand() & 0x1FF) + 1;
            void *p = malloc(s);
            if (p) {
                memset(p, 0xA5, s);
                free(p);
                atomic_fetch_add(&malloc_frees, 1);
                DBG("worker %d malloc/free size=%zu total=%ld", id, s, (long)atomic_load(&malloc_frees));
            } else {
                atomic_fetch_add(&failed_ops, 1);
                DBG("worker %d malloc failed size=%zu", id, s);
            }
        }

        random_pause();
        ++iters;
        if ((iters & 0x3FFFF) == 0) DBG("worker %d progress iters=%lu", id, iters);
    }
    DBG("worker %d exit after iters=%lu", id, iters);
    return NULL;
}

int main(int argc, char **argv) {
    unsigned long seconds = DEFAULT_SECONDS;
    if (argc > 1) seconds = strtoul(argv[1], NULL, 10);

    DBG("main start N_WORKERS=%d runtime=%lu", N_WORKERS, seconds);
    srand((unsigned int)time(NULL) ^ (unsigned)getpid());

    /* explicitly init the rwlock and check return */
    int rc = pthread_rwlock_init(&rwlock, NULL);
    if (rc != 0) {
        fprintf(stderr, "rwlock_init failed rc=%d (%s)\n", rc, strerror(rc));
        return 1;
    } else {
        DBG("rwlock_init OK");
    }

    if (sem_init(&sem, 0, 0) != 0) {
        DBG("sem_init failed errno=%d", errno);
        /* not fatal for this test, continue */
    } else {
        DBG("sem_init OK");
    }

    pthread_t threads[N_WORKERS];
    for (int i = 0; i < N_WORKERS; ++i) {
        int rc2 = pthread_create(&threads[i], NULL, worker, (void*)(intptr_t)i);
        if (rc2 != 0) {
            DBG("pthread_create worker %d failed rc=%d", i, rc2);
            atomic_fetch_add(&failed_ops, 1);
        } else {
            DBG("pthread_create worker %d OK", i);
        }
    }

    /* run for the requested duration */
    for (unsigned long t = 0; t < seconds; ++t) {
        sleep(1);
        if (t % 5 == 0) {
            fprintf(stderr, "=== stats (t=%lu) ===\n", t);
            fprintf(stderr, "global_counter: %ld\n", (long)atomic_load(&global_counter));
            fprintf(stderr, "malloc_frees: %ld\n", (long)atomic_load(&malloc_frees));
            fprintf(stderr, "failed_ops: %ld\n", (long)atomic_load(&failed_ops));
            fprintf(stderr, "lock_contentions: %ld\n", (long)atomic_load(&lock_contentions));
            fprintf(stderr, "=====================\n");
        }
    }

    keep_running = 0;
    DBG("main signaling workers to stop");

    for (int i = 0; i < N_WORKERS; ++i) {
        pthread_join(threads[i], NULL);
        DBG("joined worker %d", i);
    }

    /* cleanup */
    pthread_rwlock_destroy(&rwlock);
    sem_destroy(&sem);

    fprintf(stderr, "FINAL STATS:\n");
    fprintf(stderr, "global_counter: %ld\n", (long)atomic_load(&global_counter));
    fprintf(stderr, "malloc_frees: %ld\n", (long)atomic_load(&malloc_frees));
    fprintf(stderr, "failed_ops: %ld\n", (long)atomic_load(&failed_ops));
    fprintf(stderr, "lock_contentions: %ld\n", (long)atomic_load(&lock_contentions));

    DBG("main exit");
    return 0;
}

