/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Bounce buffers for the graphics redirection (Documentation/devel/proton-vm/design-gpu.md,
 * option B). The host listener reads what the guest hands it (a path, an ioctl argument, a
 * buffer to fill) at the address in the comm page, taken as a guest-physical address. A process
 * with its own page tables has user addresses that are not physical, so each request copies its
 * input into a physically contiguous bounce, hands the bounce's physical address, and copies the
 * output back. One bounce per request slot (the slot lock serialises its users). Where the PAL
 * cannot provide such memory (a single-process VM is identity-mapped; other PALs have no
 * redirection), the user pointers are passed through unchanged, as before.
 */

#ifdef ENABLE_GRAPHICS

#include "libos_graphics.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_vma.h"
#include "pal.h"

#define BOUNCE_SLOT_SIZE (512 * 1024)

static struct gbounce g_bounces[NUM_COMM_PAGES];
static bool g_bounce_ready;
static bool g_bounce_identity; /* no contiguous memory from the PAL: pass pointers through */
static struct libos_lock g_bounce_lock;

static int bounce_init(void) {
    if (!create_lock(&g_bounce_lock))
        return -ENOMEM;
    void* addr = NULL;
    size_t size = BOUNCE_SLOT_SIZE * NUM_COMM_PAGES;
    int ret = bkeep_mmap_any(size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | VMA_INTERNAL, /*file=*/NULL,
                             /*offset=*/0, "graphics_bounce", &addr);
    if (ret < 0)
        return ret;
    uint64_t phys = 0;
    ret = PalVirtualMemoryAllocContiguous(addr, size, PAL_PROT_READ | PAL_PROT_WRITE, &phys);
    if (ret < 0) {
        void* tmp_vma = NULL;
        if (bkeep_munmap(addr, size, /*is_internal=*/true, &tmp_vma) == 0)
            bkeep_remove_tmp_vma(tmp_vma);
        if (ret == -PAL_ERROR_NOTIMPLEMENTED) {
            g_bounce_identity = true;
            return 0;
        }
        return pal_to_unix_errno(ret);
    }
    for (size_t i = 0; i < NUM_COMM_PAGES; i++) {
        g_bounces[i].va = (char*)addr + i * BOUNCE_SLOT_SIZE;
        g_bounces[i].pa = phys + i * BOUNCE_SLOT_SIZE;
        g_bounces[i].size = BOUNCE_SLOT_SIZE;
        g_bounces[i].used = 0;
    }
    log_debug("graphics bounce buffers: %zu x %u KiB at %p (physical 0x%lx)", (size_t)NUM_COMM_PAGES,
              BOUNCE_SLOT_SIZE / 1024, addr, (unsigned long)phys);
    return 0;
}

struct gbounce* graphics_bounce(long offset) {
    if (!__atomic_load_n(&g_bounce_ready, __ATOMIC_ACQUIRE)) {
        static struct libos_lock init_lock;
        static bool init_lock_created;
        if (!init_lock_created) {
            /* the first graphics request of a process comes from one thread (the session start) */
            if (!create_lock(&init_lock))
                return NULL;
            init_lock_created = true;
        }
        lock(&init_lock);
        if (!g_bounce_ready) {
            int ret = bounce_init();
            if (ret < 0)
                log_warning("graphics bounce buffers unavailable: %s", unix_strerror(ret));
            __atomic_store_n(&g_bounce_ready, true, __ATOMIC_RELEASE);
        }
        unlock(&init_lock);
    }
    if (g_bounce_identity)
        return NULL;
    size_t slot = (size_t)offset / sizeof(comm_page_t);
    if (slot >= NUM_COMM_PAGES || !g_bounces[slot].va)
        return NULL;
    g_bounces[slot].used = 0;
    return &g_bounces[slot];
}

static void* bounce_take(struct gbounce* b, size_t size, uint64_t* out_pa) {
    size_t used = ALIGN_UP(b->used, 16);
    if (size > b->size || used > b->size - size)
        return NULL;
    b->used = used + size;
    *out_pa = b->pa + used;
    return b->va + used;
}

uint64_t graphics_bounce_in(struct gbounce* b, const void* src, size_t size) {
    if (!b)
        return (uint64_t)(uintptr_t)src;
    uint64_t pa;
    void* dst = bounce_take(b, size, &pa);
    if (!dst)
        return 0;
    memcpy(dst, src, size);
    return pa;
}

void* graphics_bounce_out(struct gbounce* b, void* user, size_t size, uint64_t* out_pa) {
    if (!b) {
        *out_pa = (uint64_t)(uintptr_t)user;
        return user;
    }
    void* va = bounce_take(b, size, out_pa);
    if (va)
        memcpy(va, user, size); /* in-out arguments keep the application's input */
    return va;
}

size_t graphics_bounce_room(struct gbounce* b) {
    if (!b)
        return SIZE_MAX;
    size_t used = ALIGN_UP(b->used, 16);
    return used < b->size ? b->size - used : 0;
}

#endif /* ENABLE_GRAPHICS */
