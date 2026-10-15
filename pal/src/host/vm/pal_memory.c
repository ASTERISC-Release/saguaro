/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * This file contains APIs that allocate, free or protect virtual memory.
 */

#include "api.h"
#include "kernel_memory.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_internal.h"

#if defined(__x86_64__) || defined(__aarch64__)
static struct memory_address_space* current_address_space(void) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    return tcb ? tcb->kernel_thread.address_space : NULL;
}
#else
/* Per-process address spaces (the experimental multiprocess support) exist only on x86_64: other
 * architectures have no current address space, so the memory_address_space_*() calls below are
 * never reached there. */
#define current_address_space() ((void*)NULL)
#define memory_address_space_alloc(...) (-PAL_ERROR_NOTIMPLEMENTED)
#define memory_address_space_free(...) (-PAL_ERROR_NOTIMPLEMENTED)
#define memory_address_space_protect(...) (-PAL_ERROR_NOTIMPLEMENTED)
struct memory_address_space;
#endif

int _PalVirtualMemoryAlloc(void* addr, size_t size, pal_prot_flags_t prot) {
    assert(WITHIN_MASK(prot, PAL_PROT_MASK));
    assert(addr);

    bool read    = !!(prot & PAL_PROT_READ);
    bool write   = !!(prot & (PAL_PROT_WRITE | PAL_PROT_WRITECOPY));
    bool execute = !!(prot & PAL_PROT_EXEC);
    struct memory_address_space* address_space = current_address_space();
    if (address_space)
        return memory_address_space_alloc(address_space, addr, size, read, write, execute);
    return memory_alloc(addr, size, read, write, execute);
}

int _PalVirtualMemoryFree(void* addr, size_t size) {
    assert(addr);
    struct memory_address_space* address_space = current_address_space();
    if (address_space)
        return memory_address_space_free(address_space, addr, size);
    return memory_free(addr, size);
}

int _PalVirtualMemoryProtect(void* addr, size_t size, pal_prot_flags_t prot) {
    assert(WITHIN_MASK(prot, PAL_PROT_MASK));
    assert(addr);

    bool read    = !!(prot & PAL_PROT_READ);
    bool write   = !!(prot & (PAL_PROT_WRITE | PAL_PROT_WRITECOPY));
    bool execute = !!(prot & PAL_PROT_EXEC);
    struct memory_address_space* address_space = current_address_space();
    if (address_space)
        return memory_address_space_protect(address_space, addr, size, read, write, execute);
    return memory_protect(addr, size, read, write, execute);
}

unsigned long _PalMemoryQuota(void) {
    return g_pal_public_state.memory_address_end - g_pal_public_state.memory_address_start;
}

/* Ranges the parent's application uses, given by _PalProcessCreate (sorted from high to low); a
 * child's early allocations avoid them so that the checkpoint can be restored at its addresses. */
static const uintptr_t (*g_reserved_ranges)[2] __attribute__((section(".process_data")));
static size_t g_reserved_ranges_count __attribute__((section(".process_data")));
static size_t g_reserved_ranges_next __attribute__((section(".process_data")));

void vm_set_reserved_ranges(const uintptr_t (*ranges)[2], size_t count) {
    g_reserved_ranges = ranges;
    g_reserved_ranges_count = count;
    g_reserved_ranges_next = 0;
}

void pal_read_next_reserved_range(uintptr_t last_range_start, uintptr_t* out_next_range_start,
                                  uintptr_t* out_next_range_end) {
    __UNUSED(last_range_start);

    if (g_reserved_ranges_next >= g_reserved_ranges_count) {
        *out_next_range_start = 0;
        *out_next_range_end = 0;
        return;
    }
    const uintptr_t* range = g_reserved_ranges[g_reserved_ranges_next++];
    assert(range[0] <= range[1] && range[1] <= last_range_start);
    *out_next_range_start = range[0];
    *out_next_range_end = range[1];
}
