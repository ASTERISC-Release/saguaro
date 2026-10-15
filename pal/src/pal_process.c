/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This source file contains functions to create a child process and terminate the running process.
 * Child does not inherit any objects or memory from its parent process. A parent process may not
 * modify the execution of its children. It can wait for a child to exit using its handle. Also,
 * parent and child may communicate through I/O streams provided by the parent to the child at
 * creation.
 */

#include "pal.h"
#include "pal_internal.h"

int PalProcessCreate(const char** args, uintptr_t (*reserved_mem_ranges)[2],
                     size_t reserved_mem_ranges_len, PAL_HANDLE* out_handle) {
    *out_handle = NULL;
    int ret = _PalProcessCreate(args, reserved_mem_ranges, reserved_mem_ranges_len, out_handle);
    PAL_TRACK_HANDLE(ret, *out_handle);
    return ret;
}

noreturn void PalProcessExit(int exitcode) {
    _PalProcessExit(exitcode);
}

int PalFutexSharedAdd(uint32_t* addr, uint32_t val, uint32_t bitset, PAL_HANDLE event,
                      void** out_waiter) {
#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
    return _PalFutexSharedAdd(addr, val, bitset, event, out_waiter);
#else
    __UNUSED(addr);
    __UNUSED(val);
    __UNUSED(bitset);
    __UNUSED(event);
    __UNUSED(out_waiter);
    return -PAL_ERROR_NOTIMPLEMENTED;
#endif
}

int PalFutexSharedRemove(void* waiter, bool* out_woken) {
#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
    return _PalFutexSharedRemove(waiter, out_woken);
#else
    __UNUSED(waiter);
    __UNUSED(out_woken);
    return -PAL_ERROR_NOTIMPLEMENTED;
#endif
}

int PalFutexSharedWake(uint32_t* addr, uint32_t bitset, int count, int* out_woken) {
#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
    return _PalFutexSharedWake(addr, bitset, count, out_woken);
#else
    __UNUSED(addr);
    __UNUSED(bitset);
    __UNUSED(count);
    __UNUSED(out_woken);
    return -PAL_ERROR_NOTIMPLEMENTED;
#endif
}
