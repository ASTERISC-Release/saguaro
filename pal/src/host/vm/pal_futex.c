/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Futexes shared between the processes of the guest (x86_64, multiprocess support only).
 *
 * Each process has its own LibOS and so its own futex table, keyed by virtual address. A futex
 * word in memory that several processes map (MAP_SHARED) needs a key that is the same in all of
 * them: the physical address of the word. The waiters live in one list in PAL memory, which every
 * address space maps at the same address; a waiter carries the LibOS thread's event, and a wake
 * from any process sets it. The LibOS keeps timeouts, signals and the private futexes.
 */

#include "api.h"
#include "list.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "spinlock.h"

#include "kernel_memory.h"
#include "kernel_thread.h"

DEFINE_LIST(futex_shared_waiter);
DEFINE_LISTP(futex_shared_waiter);
struct futex_shared_waiter {
    uintptr_t key; /* physical address of the futex word */
    uint32_t bitset;
    bool woken;
    PAL_HANDLE event;
    struct memory_address_space* space;
    LIST_TYPE(futex_shared_waiter) list;
};

static LISTP_TYPE(futex_shared_waiter) g_futex_shared_waiters = LISTP_INIT;
static spinlock_t g_futex_shared_lock = INIT_SPINLOCK_UNLOCKED;

static struct memory_address_space* current_space(void) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    return tcb ? tcb->kernel_thread.address_space : NULL;
}

static int futex_shared_key(uint32_t* addr, uintptr_t* out_key) {
    if (!memory_frame_pool_active())
        return -PAL_ERROR_NOTIMPLEMENTED;
    if (!IS_ALIGNED_PTR(addr, sizeof(*addr)))
        return -PAL_ERROR_INVAL;
    /* a first read materializes a demand-zero page (the fault handler maps it) */
    (void)__atomic_load_n(addr, __ATOMIC_RELAXED);
    return memory_address_space_translate((uintptr_t)addr, out_key);
}

int _PalFutexSharedAdd(uint32_t* addr, uint32_t val, uint32_t bitset, PAL_HANDLE event,
                       void** out_waiter) {
    uintptr_t key;
    int ret = futex_shared_key(addr, &key);
    if (ret < 0)
        return ret;

    struct futex_shared_waiter* waiter = malloc(sizeof(*waiter));
    if (!waiter)
        return -PAL_ERROR_NOMEM;
    waiter->key = key;
    waiter->bitset = bitset;
    waiter->woken = false;
    waiter->event = event;
    waiter->space = current_space();
    INIT_LIST_HEAD(waiter, list);

    spinlock_lock(&g_futex_shared_lock);
    if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != val) {
        spinlock_unlock(&g_futex_shared_lock);
        free(waiter);
        return -PAL_ERROR_TRYAGAIN;
    }
    LISTP_ADD_TAIL(waiter, &g_futex_shared_waiters, list);
    spinlock_unlock(&g_futex_shared_lock);

    *out_waiter = waiter;
    return 0;
}

int _PalFutexSharedRemove(void* waiter_ptr, bool* out_woken) {
    struct futex_shared_waiter* waiter = waiter_ptr;

    spinlock_lock(&g_futex_shared_lock);
    if (!LIST_EMPTY(waiter, list))
        LISTP_DEL_INIT(waiter, &g_futex_shared_waiters, list);
    bool woken = waiter->woken;
    spinlock_unlock(&g_futex_shared_lock);

    free(waiter);
    *out_woken = woken;
    return 0;
}

int _PalFutexSharedWake(uint32_t* addr, uint32_t bitset, int count, int* out_woken) {
    uintptr_t key;
    int ret = futex_shared_key(addr, &key);
    if (ret < 0)
        return ret;

    int woken = 0;
    struct futex_shared_waiter* waiter;
    struct futex_shared_waiter* tmp;
    spinlock_lock(&g_futex_shared_lock);
    LISTP_FOR_EACH_ENTRY_SAFE(waiter, tmp, &g_futex_shared_waiters, list) {
        if (waiter->key != key || !(waiter->bitset & bitset))
            continue;
        LISTP_DEL_INIT(waiter, &g_futex_shared_waiters, list);
        waiter->woken = true;
        /* under the lock: the waiter frees itself only after taking it */
        pal_common_event_set(waiter->event);
        /* as Linux, a count of 0 still wakes one waiter */
        if (++woken >= count)
            break;
    }
    spinlock_unlock(&g_futex_shared_lock);

    *out_woken = woken;
    return 0;
}

void vm_futex_shared_purge(struct memory_address_space* space) {
    struct futex_shared_waiter* waiter;
    struct futex_shared_waiter* tmp;
    spinlock_lock(&g_futex_shared_lock);
    LISTP_FOR_EACH_ENTRY_SAFE(waiter, tmp, &g_futex_shared_waiters, list) {
        if (waiter->space != space)
            continue;
        LISTP_DEL_INIT(waiter, &g_futex_shared_waiters, list);
        free(waiter);
    }
    spinlock_unlock(&g_futex_shared_lock);
}
