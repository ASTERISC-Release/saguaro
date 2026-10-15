/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "libos_lock.h"
#include "log.h"

/*
 * The shared Tirith graphics ABI -- the comm page, the request ids and the
 * fallback arena constants -- defined once in graphics_comm_abi.h (also
 * consumed by the preload library and the QEMU host listener). This header
 * adds the LibOS-side declarations; everything below is compiled in only with
 * -Dgraphics=enabled (which defines ENABLE_GRAPHICS for the LibOS).
 */
#include "graphics_comm_abi.h"

#ifdef ENABLE_GRAPHICS

/* The guest installs forwarded host fds at (host fd + GRAPHICS_FS_OFFSET). */
#define GRAPHICS_HOST_FD_OFFSET GRAPHICS_FS_OFFSET

/* Both Integrated and Discrete GPUs should be supported */
#define iGPUNODE_PATH "/dev/dri/renderD128"
#define dGPUNODE_PATH "/dev/dri/renderD129"

static void* __attribute__((unused)) DATA_REGION = (void*)GRAPHICS_GEM_DATA_START;
static uint64_t __attribute__((unused)) special_allocation = 0x123456789ULL;
static uint64_t __attribute__((unused)) fs_offset = GRAPHICS_FS_OFFSET;

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

/* Drops a stale TYPE_HOST handle at the guest slot of a host fd the host just returned (libos_open.c). */
int graphics_drop_stale_host_handle(int fd);

/* Set once the application has mapped the comm window at COMM_ADDR (libos_syscall_mmap()). */
extern bool g_graphics_comm_mapped;

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