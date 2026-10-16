/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
#include <stdlib.h>

#include "./../../../libos/include/libos_graphics.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "linux_abi/errors.h"
#include "cpu.h"

#ifdef ENABLE_GRAPHICS

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
            log_always("[GRAPHICS_LOCK] New thread accessing acquire: tid=%u, total_threads=%d",
                       tid, expected + 1);
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
            log_always("[GRAPHICS_LOCK] ERROR: all %d slots exhausted, tid=%u", NUM_COMM_LOCKS,
                       tid);
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
            log_always("[GRAPHICS_LOCK] LOG: %dth slot claimed for tid = %u - %d threads in total",
                       expected, tid, expected + 1);
            return expected;
        }
        /* CAS failed, retry */
    }
}
#endif /* MULTI_SLOT */

/*
 * Request slots (NUM_COMM_PAGES of them, each with its own listener thread). A thread takes the
 * first free slot (an atomic flag; slot 0 first, so a single-threaded program always uses slot 0)
 * and holds it for one request. When all slots are busy, it queues on the lock of slot
 * (tid % NUM_COMM_PAGES) and takes that slot as soon as its holder lets it go. A thread blocked in
 * a long host call (a GPU wait) therefore stalls only the threads queued behind its slot, and only
 * when all slots are in use.
 */
static uint32_t g_slot_busy_private[NUM_COMM_PAGES];
static bool g_slot_queued_holder[NUM_COMM_PAGES];

/* The slot-busy flags live in the comm page's guest-private area once the process has mapped it
 * (graphics_comm_abi.h): one physical page for the whole guest, so the four request slots are
 * mutually exclusive across processes (several Wine processes share one session,
 * design-gpu.md). Before that mapping (nothing uses slots then) the private array stands in;
 * a process always acquires and releases through the same array, since the mapping happens
 * before its first request. */
static volatile uint32_t* slot_busy_flags(void) {
    if (__atomic_load_n(&g_graphics_comm_mapped, __ATOMIC_ACQUIRE)) {
        graphics_guest_state_t* state =
            (graphics_guest_state_t*)(uintptr_t)(COMM_ADDR + GRAPHICS_GUEST_STATE_OFFSET);
        return state->slot_busy;
    }
    return g_slot_busy_private;
}

long libos_syscall_acquire_libos_lock(void) {
#if TRACK_ACQUIRE_THREADS
    track_thread_tid();
#endif
    _Static_assert(NUM_COMM_PAGES * sizeof(comm_page_t) <= 512,
                   "the request slots must end before the input ring");

    volatile uint32_t* busy = slot_busy_flags();
    for (int i = 0; i < NUM_COMM_PAGES; i++) {
        if (!__atomic_load_n(&busy[i], __ATOMIC_RELAXED) &&
                !__atomic_exchange_n(&busy[i], 1, __ATOMIC_ACQUIRE))
            return i * sizeof(comm_page_t);
    }

    int index = get_cur_tid() % NUM_COMM_PAGES;
    ensure_lock_ready(&comm_locks[index]);
    lock(&comm_locks[index]);
    /* The VM PAL schedules cooperatively: yield, or the slot's holder may never run again on
     * this vCPU (seen with four Mesa shader-cache threads and one vCPU). */
    while (__atomic_exchange_n(&busy[index], 1, __ATOMIC_ACQUIRE)) {
        CPU_RELAX();
        PalThreadYieldExecution();
    }
    g_slot_queued_holder[index] = true;
    return index * sizeof(comm_page_t);
}

long libos_syscall_relinquish_libos_lock(long offset) {
    int index = offset / sizeof(comm_page_t);
    if (index < 0 || index >= NUM_COMM_PAGES)
        return -EINVAL;
    bool queued = g_slot_queued_holder[index];
    g_slot_queued_holder[index] = false;
    __atomic_store_n(&slot_busy_flags()[index], 0, __ATOMIC_RELEASE);
    if (queued)
        unlock(&comm_locks[index]);
    return 0;
}

#else /* !ENABLE_GRAPHICS */

/* The syscall tables always reference these two (custom) syscalls; without graphics they
 * simply do not exist. */
long libos_syscall_acquire_libos_lock(void) {
    return -ENOSYS;
}

long libos_syscall_graphics_physmem(size_t size, uint64_t* out_pa) {
    __UNUSED(size);
    __UNUSED(out_pa);
    return -ENOSYS;
}

long libos_syscall_relinquish_libos_lock(long offset) {
    __UNUSED(offset);
    return -ENOSYS;
}

#endif /* ENABLE_GRAPHICS */
