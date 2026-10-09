#include <stdlib.h>

#include "./../../../libos/include/libos_sg.h"
#include "libos_thread.h"

/* Set to 1 to give each thread its own slot; 0 = all threads share slot 0 */
#define MULTI_SLOT 0

/* Set to 1 to enable tracking of unique threads accessing acquire_libos_lock */
#define TRACK_ACQUIRE_THREADS 0

#if TRACK_ACQUIRE_THREADS
#define MAX_TRACKED_THREADS 64

static unsigned int tracked_tids[MAX_TRACKED_THREADS];
static int total_unique_threads = 0;

static void track_thread_tid(void) {
    unsigned int tid = get_cur_tid();

    /* First pass: check if TID already exists (lock-free read) */
    int current_total = __atomic_load_n(&total_unique_threads, __ATOMIC_ACQUIRE);
    for (int i = 0; i < current_total; i++) {
        if (__atomic_load_n(&tracked_tids[i], __ATOMIC_ACQUIRE) == tid) {
            return; /* Already tracked */
        }
    }

    /* Try to claim a slot atomically using CAS on total_unique_threads */
    while (1) {
        int expected = __atomic_load_n(&total_unique_threads, __ATOMIC_ACQUIRE);

        if (expected >= MAX_TRACKED_THREADS) {
            return; /* Array full */
        }

        /* Double-check the TID isn't already present (another thread may have added it) */
        for (int i = 0; i < expected; i++) {
            if (__atomic_load_n(&tracked_tids[i], __ATOMIC_ACQUIRE) == tid) {
                return; /* Already tracked */
            }
        }

        /* Try to atomically increment the counter to claim slot at index 'expected' */
        if (__atomic_compare_exchange_n(&total_unique_threads, &expected, expected + 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            /* Successfully claimed slot 'expected', now store our TID */
            __atomic_store_n(&tracked_tids[expected], tid, __ATOMIC_RELEASE);
            log_always("[SG_LOCK] New thread accessing acquire: tid=%u, total_threads=%d", tid,
                       expected + 1);
            return;
        }
        /* CAS failed, another thread claimed the slot - retry */
    }
}
#endif /* TRACK_ACQUIRE_THREADS */

/* Number of per-thread comm page locks */
#define NUM_COMM_LOCKS 32

static struct libos_lock comm_locks[NUM_COMM_LOCKS];

#if MULTI_SLOT
/* Dynamic TID-to-slot mapping */
static unsigned int
    slot_tids[NUM_COMM_LOCKS];     /* slot_tids[i] = TID assigned to slot i, 0 = unused */
static int num_assigned_slots = 0; /* atomically managed counter */

/*
 * Returns the slot index for the calling thread's TID.
 * If the TID has not been seen before, atomically assigns the next available slot.
 * Returns -1 if all slots are exhausted.
 */
static int get_slot_for_tid(unsigned int tid) {
    /* Fast path: plain reads — slot_tids entries are immutable once written */
    int current_count = num_assigned_slots;
    for (int i = 0; i < current_count; i++) {
        if (slot_tids[i] == tid)
            return i;
    }

    /* Slow path: claim a new slot via CAS loop (only hit once per thread) */
    while (1) {
        int expected = __atomic_load_n(&num_assigned_slots, __ATOMIC_ACQUIRE);

        if (expected >= NUM_COMM_LOCKS) {
            log_always("[SG_LOCK] ERROR: all %d slots exhausted, tid=%u", NUM_COMM_LOCKS, tid);
            abort();
            return -1;
        }

        /* Re-check: another thread may have added our TID concurrently */
        for (int i = 0; i < expected; i++) {
            if (slot_tids[i] == tid) {
                return i;
            }
        }

        /* Try to claim slot at index 'expected' */
        if (__atomic_compare_exchange_n(&num_assigned_slots, &expected, expected + 1,
                                        /*weak=*/0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            slot_tids[expected] = tid;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            log_always("[SG_LOCK] LOG: %dth slot claimed for tid = %u - %d threads in total",
                       expected, tid, expected + 1);
            return expected;
        }
        /* CAS failed, retry */
    }
}
#endif /* MULTI_SLOT */

long libos_syscall_acquire_libos_lock(void) {
#if TRACK_ACQUIRE_THREADS
    track_thread_tid();
#endif

#if MULTI_SLOT
    unsigned int tid = get_cur_tid();
    int index        = get_slot_for_tid(tid);

    if (index < 0)
        return -1;
#else
    int index = 0;
#endif

    /* Ensure the lock is initialized, then acquire it */
    ensure_lock_ready(&comm_locks[index]);
    lock(&comm_locks[index]);

    return index * sizeof(comm_page_t);
}

long libos_syscall_relinquish_libos_lock(long offset) {
    int index = offset / sizeof(comm_page_t);
    unlock(&comm_locks[index]);
    return 0;
}
