/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "libos_lock.h"
#include "log.h"

/* Everything below is Tirith graphics code, compiled in only with -Dgraphics=enabled
 * (which defines ENABLE_GRAPHICS for the LibOS). */
#ifdef ENABLE_GRAPHICS

#define GRAPHICS_HOST_FD_OFFSET 500

#define GRAPHICS_GEM_DATA_START      0x100000000ULL
#define GRAPHICS_GEM_DATA_SIZE       (3ULL * 1024 * 1024 * 1024)
#define GRAPHICS_GEM_HUGEPAGE_START  0x200000000ULL
#define GRAPHICS_GEM_HUGEPAGE_SIZE   (1ULL * 1024 * 1024 * 1024)

#define NUM_COMM_PAGES 1

typedef struct {
    volatile uint64_t magic;
    volatile uint64_t req_bit;
    volatile uint64_t ret;
    volatile uint64_t p1;
    volatile uint64_t p2;
    volatile uint64_t p3;
    volatile uint64_t p4;
    volatile uint64_t p5;
} comm_page_t;

_Static_assert(offsetof(comm_page_t, ret) == 16,
               "graphics response must stay in the hot cache line");
_Static_assert(offsetof(comm_page_t, p5) == 56,
               "common graphics payload must fit one cache line");
_Static_assert(sizeof(comm_page_t) == 64, "graphics communication ABI must fit one cache line");

extern struct libos_lock graphics_comm_lock;

/* Whether the graphics host comm window at COMM_ADDR can exist at all in this deployment: it is
 * attached by the host only under the VM/TDX PAL (full Tirith deployment with a qemu
 * sg-listener); under the plain `Linux` PAL nothing ever maps it. Defined (with the full
 * rationale) in libos_open.c next to the other graphics entry point. Every comm_page() writer whose
 * predicate is path-based -- openat's is_graphics_host_path() (via that same helper) and
 * stat()/newfstatat()'s strstr() checks -- must consult this *before* touching the window:
 * comm_sync_notify()'s `magic` check cannot cover it, because every call site writes c->p1..p3
 * first, so an unmapped window faults at COMM_ADDR + 0x18 on the first write. */
bool graphics_redirect_enabled(void);

/* Set once the application has mapped the comm window at COMM_ADDR (libos_syscall_mmap()). */
extern bool g_graphics_comm_mapped;

#define COMM_ADDR  0xf00000ULL
#define COMM_MAGIC 0x1234567812345678ULL
static uint64_t __attribute__((unused)) LOG_MMAP_EVENT = 1;
static uint64_t __attribute__((unused)) SETUP_DATA = 2;
static uint64_t __attribute__((unused)) GEM_ALLOCATION = 3;
static uint64_t __attribute__((unused)) FSTAT = 4;
static uint64_t __attribute__((unused)) IOCTL = 5;
static uint64_t __attribute__((unused)) OPEN = 6;
static uint64_t __attribute__((unused)) FCNTL = 7;
static uint64_t __attribute__((unused)) READLINK = 8;
static uint64_t __attribute__((unused)) NEWFSTAT = 9;
static uint64_t __attribute__((unused)) GETDENT = 10;
static uint64_t __attribute__((unused)) DUP = 11;
static uint64_t __attribute__((unused)) DUP_IDENTITY = 30;
static uint64_t __attribute__((unused)) X11_SETUP = 12;
static uint64_t __attribute__((unused)) X11_PRESENT = 13;
static uint64_t __attribute__((unused)) CLOSE = 14;

static uint64_t __attribute__((unused)) LSEEK = 22;
static uint64_t __attribute__((unused)) READ = 24;
static uint64_t __attribute__((unused)) WRITE = 25;
static uint64_t __attribute__((unused)) SYSV_SEMGET = 34;
static uint64_t __attribute__((unused)) SYSV_SEMCTL = 35;
static uint64_t __attribute__((unused)) SYSV_SEMOP = 36;
static uint64_t __attribute__((unused)) HOST_FLOCK = 37;
static uint64_t __attribute__((unused)) HOST_FTRUNCATE = 38;

static size_t __attribute__((unused)) special_allocation = 0x123456789ULL;
static uint64_t __attribute__((unused)) fs_offset = 500;
/* Both Integrated and Discrete GPUs should be supported */
#define iGPUNODE_PATH "/dev/dri/renderD128"
#define dGPUNODE_PATH "/dev/dri/renderD129"

static void* __attribute__((unused)) DATA_REGION = (void*)GRAPHICS_GEM_DATA_START;
static const size_t __attribute__((unused)) DATA_SIZE = GRAPHICS_GEM_DATA_SIZE;
static inline comm_page_t* comm_page(long offset) {
    return (comm_page_t*)(uintptr_t)(COMM_ADDR + offset);
}

static __attribute__((unused)) uint64_t comm_sync_notify(comm_page_t* c) {
    if (!c || c->magic != COMM_MAGIC) {
        return 0;
    }
    /* Publish the request payload and request bit before polling for host
     * completion. The host accesses the same memory through a separate mapping;
     * volatile alone does not define an inter-thread/process synchronization
     * relationship. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* The dedicated host listener normally completes these shared-memory requests immediately.
     * A minimal load/test loop avoids AMD KVM's PAUSE-filter path and minimizes round-trip
     * latency; this path is used only while an graphics host request is outstanding. */
    while (__atomic_load_n(&c->req_bit, __ATOMIC_ACQUIRE) != 0) {
    }

    return c->ret;
}

static inline __attribute__((unused)) void ensure_lock_ready(struct libos_lock *lock){
    if(lock_created(lock))
        return;
    if(!(create_lock(lock))){
        assert(false);
    }else {
        // log_always("[Gramine] Global lock initialized\n");
    }
}

#endif /* ENABLE_GRAPHICS */
