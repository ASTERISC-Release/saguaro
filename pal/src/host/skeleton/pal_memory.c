/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains APIs that allocate, free or protect virtual memory.
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"

int _PalVirtualMemoryAlloc(void* addr, uint64_t size, pal_prot_flags_t prot) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalVirtualMemoryFree(void* addr, uint64_t size) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalVirtualMemoryProtect(void* addr, uint64_t size, pal_prot_flags_t prot) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

unsigned long _PalMemoryQuota(void) {
    return 0;
}

void pal_read_next_reserved_range(uintptr_t last_range_start, uintptr_t* out_next_range_start,
                                  uintptr_t* out_next_range_end) {
    *out_next_range_start = 0;
    *out_next_range_end = 0;
}

int _PalVirtualMemoryAllocContiguous(void* addr, size_t size, pal_prot_flags_t prot,
                                     uint64_t* out_phys) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(prot);
    __UNUSED(out_phys);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalVirtualMemoryAlias(void* addr, void* src, size_t size, pal_prot_flags_t prot) {
    __UNUSED(addr);
    __UNUSED(src);
    __UNUSED(size);
    __UNUSED(prot);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalVirtualMemoryDiscard(void* addr, size_t size) {
    __UNUSED(addr);
    __UNUSED(size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}
