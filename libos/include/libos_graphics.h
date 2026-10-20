/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "libos_lock.h"
#include "log.h"
#include "pal.h"

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
/* The comm page mapped in this process by the LibOS itself, for a process that never ran the
 * graphics library (the wineserver receiving a host handle over SCM_RIGHTS): every forwarded
 * operation on a host handle needs it. 0 when it is mapped (already or now), -errno otherwise. */
int graphics_ensure_comm_mapped(void);
/* Asks the listener to dup() the host fd behind `fd` (a TYPE_HOST or TYPE_HOST_IDENTITY descriptor,
 * host fd + offset) and returns the new descriptor number (host fd + offset) or -errno; the caller
 * installs the handle. */
long graphics_host_dup_request(int fd, int type);

/* Set once the application has mapped the comm window at COMM_ADDR (libos_syscall_mmap()). */
extern bool g_graphics_comm_mapped;

/* Bounce buffers for what the listener reads by physical address (sys/libos_graphics_bounce.c,
 * dev docs: proton-vm/design-gpu.md). graphics_bounce(slot offset) returns the slot's
 * bounce, reset, or NULL when user pointers can be passed through (identity-mapped memory). */
struct gbounce {
    char* va;
    uint64_t pa;
    size_t size;
    size_t used;
};
struct gbounce* graphics_bounce(long offset);
/* copies `size` bytes in; returns the physical address to hand the host (0: no room) */
uint64_t graphics_bounce_in(struct gbounce* b, const void* src, size_t size);
/* reserves `size` bytes for an output (or in-out) argument, seeded with the user's bytes;
 * returns the LibOS address to copy back from (equal to `user` when passed through), NULL: no
 * room; `*out_pa` is what the host gets */
void* graphics_bounce_out(struct gbounce* b, void* user, size_t size, uint64_t* out_pa);
/* bytes still free in the bounce (SIZE_MAX when passing through) */
size_t graphics_bounce_room(struct gbounce* b);

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
    /* A request that takes long (a GPU wait) must not keep the vCPU from the guest's other
     * threads: with one vCPU, the thread that would signal the awaited event could not run until
     * the wait timed out. So after a short spin (about 50-100 us), yield between polls. */
    unsigned long spins = 0;
    while (__atomic_load_n(&c->req_bit, __ATOMIC_ACQUIRE) != 0) {
        if (++spins > 100000)
            PalThreadYieldExecution();
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