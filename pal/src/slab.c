/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains implementation of PAL's internal memory allocator.
 */

#include "api.h"
#include "asan.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "spinlock.h"

#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
#include "kernel_memory.h"
static bool g_slab_mgr_use_frame_pool;
#endif

static spinlock_t g_slab_mgr_lock = INIT_SPINLOCK_UNLOCKED;

#define SYSTEM_LOCK()   spinlock_lock(&g_slab_mgr_lock)
#define SYSTEM_UNLOCK() spinlock_unlock(&g_slab_mgr_lock)
#define SYSTEM_LOCKED() spinlock_is_locked(&g_slab_mgr_lock)

static inline void* system_mem_alloc(size_t size);
static inline void system_mem_free(void* addr, size_t size);
#define system_malloc(size) system_mem_alloc(size)
#define system_free(addr, size) system_mem_free(addr, size)

#include "slabmgr.h"

#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
static SLAB_MGR g_slab_mgr_legacy;
#endif

static inline void* system_mem_alloc(size_t size) {
    void* addr = NULL;

    size = ALLOC_ALIGN_UP(size);

#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
    if (g_slab_mgr_use_frame_pool) {
        int ret = memory_frame_alloc_contiguous(size, &addr);
        return ret < 0 ? NULL : addr;
    }
#endif

    int ret = pal_internal_memory_alloc(size, &addr);
    if (ret < 0) {
        return NULL;
    }

#ifdef ASAN
    asan_poison_region((uintptr_t)addr, size, ASAN_POISON_HEAP_LEFT_REDZONE);
#endif

    return addr;
}

static inline void system_mem_free(void* addr, size_t size) {
    if (!addr)
        return;

    size = ALLOC_ALIGN_UP(size);

#ifdef ASAN
    /* Unpoison the memory before unmapping it */
    asan_unpoison_region((uintptr_t)addr, size);
#endif

#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
    if (g_slab_mgr_use_frame_pool) {
        if (memory_frame_is_alias_pointer(addr)) {
            int ret = memory_frame_free_contiguous(addr, size);
            if (ret < 0) {
                log_error("freeing PAL frame-pool allocation failed: %s", pal_strerror(ret));
                _PalProcessExit(1);
            }
        }
        /* Pre-feature large allocations remain mapped for any root that inherited them. They are
         * finite bootstrap leftovers and cannot safely be freed through a child-private bkeep. */
        return;
    }
#endif

    int ret = pal_internal_memory_free(addr, size);
    if (ret < 0) {
        log_error("freeing memory failed: %s", pal_strerror(ret));
        _PalProcessExit(1);
    }
}

static SLAB_MGR g_slab_mgr = NULL;

#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
/* Allocations a child process's PAL makes while it starts (its environment and arguments, CPU
 * information): nothing outside the process refers to them, but they are in the PAL heap that all
 * processes share, so they are freed when the process exits (pal_startup_allocs_free()). */
#define STARTUP_ALLOCS_MAX 128
static void* g_startup_allocs[STARTUP_ALLOCS_MAX] PAL_PROCESS_DATA;
static size_t g_startup_allocs_cnt PAL_PROCESS_DATA;
static bool g_startup_allocs_recording PAL_PROCESS_DATA;

static void startup_alloc_note(void* ptr) {
    if (ptr && g_startup_allocs_recording && g_startup_allocs_cnt < STARTUP_ALLOCS_MAX)
        g_startup_allocs[g_startup_allocs_cnt++] = ptr;
}

static void startup_alloc_forget(void* ptr) {
    for (size_t i = g_startup_allocs_cnt; i > 0; i--) {
        if (g_startup_allocs[i - 1] == ptr) {
            g_startup_allocs[i - 1] = g_startup_allocs[--g_startup_allocs_cnt];
            return;
        }
    }
}

void pal_startup_allocs_record(bool on) {
    if (on)
        g_startup_allocs_cnt = 0; /* the copied process data holds the parent's list */
    g_startup_allocs_recording = on;
}

void pal_startup_allocs_forget(void* ptr) {
    if (ptr)
        startup_alloc_forget(ptr);
}

void pal_startup_allocs_free(void) {
    g_startup_allocs_recording = false;
    while (g_startup_allocs_cnt)
        free(g_startup_allocs[g_startup_allocs_cnt - 1]); /* free() removes it from the list */
}
#endif

void init_slab_mgr(void) {
    assert(!g_slab_mgr);

    g_slab_mgr = create_slab_mgr();
    if (!g_slab_mgr)
        INIT_FAIL("cannot initialize slab manager");
}

#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
int pal_enable_shared_frame_slab_allocator(void) {
    if (!g_slab_mgr || g_slab_mgr_use_frame_pool || g_slab_mgr_legacy)
        return -PAL_ERROR_INVAL;

    g_slab_mgr_legacy = g_slab_mgr;
    g_slab_mgr_use_frame_pool = true;
    g_slab_mgr = create_slab_mgr();
    if (g_slab_mgr)
        return 0;

    g_slab_mgr_use_frame_pool = false;
    g_slab_mgr = g_slab_mgr_legacy;
    g_slab_mgr_legacy = NULL;
    return -PAL_ERROR_NOMEM;
}
#endif

void* malloc(size_t size) {
    void* ptr = slab_alloc(g_slab_mgr, size);

#ifdef DEBUG
    /* In debug builds, try to break code that uses uninitialized heap
     * memory by explicitly initializing to a non-zero value. */
    if (ptr)
        memset(ptr, 0xa5, size);
#endif

#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
    startup_alloc_note(ptr);
#endif
    return ptr;
}

void* calloc(size_t num, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(num, size, &total))
        return NULL;

    void* ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void free(void* ptr) {
    if (!ptr)
        return;
#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
    if (g_startup_allocs_cnt)
        startup_alloc_forget(ptr);
    if (g_slab_mgr_legacy && slab_mgr_owns(g_slab_mgr_legacy, ptr)) {
        slab_free(g_slab_mgr_legacy, ptr);
        return;
    }
#endif
    slab_free(g_slab_mgr, ptr);
}
