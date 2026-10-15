/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2022 Integritee AG
 *                    Frieder Paape <frieder@integritee.network>
 */

/*
 * Implements mlock, mlock2, munlock, mlockall, munlockall (lock and unlock memory). These syscalls
 * are stubbed to always return success -- Gramine cannot guarantee that the host OS will perform
 * lock/unlock anyway, and a malicious OS can still swap pages anyway.
 *
 * This (dummy) functionality is required by .NET workloads.
 */

#include "api.h"
#include "libos_table.h"
#include "libos_vma.h"
#include "linux_abi/errors.h"
#include "linux_abi/memory.h"

/* The range a lock call names must be mapped: ENOMEM otherwise (LTP munlock02), as in the kernel;
 * the length is rounded out to whole pages from the page holding `start`. */
static long check_lock_range(unsigned long start, size_t len) {
    if (!access_ok((void*)start, len))
        return -EINVAL;
    if (!len)
        return 0;
    uintptr_t begin = ALLOC_ALIGN_DOWN(start);
    uintptr_t end = ALLOC_ALIGN_UP(start + len);
    if (!is_in_adjacent_user_vmas((void*)begin, end - begin, /*prot=*/0))
        return -ENOMEM;
    return 0;
}

long libos_syscall_mlock(unsigned long start, size_t len) {
    return check_lock_range(start, len);
}

long libos_syscall_munlock(unsigned long start, size_t len) {
    return check_lock_range(start, len);
}

long libos_syscall_mlockall(int flags) {
    int unknown = flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT);
    if (unknown != 0) {
        log_warning("Syscall mlockall was called with unknown flag(s): %#x\n", unknown);
        return -EINVAL;
    }

    return 0;
}

long libos_syscall_munlockall(void) {
    return 0;
}

long libos_syscall_mlock2(unsigned long start, size_t len, int flags) {
    int unknown = flags & ~MLOCK_ONFAULT;
    if (unknown != 0) {
        log_warning("Syscall mlock2 was called with unknown flag(s): %#x\n", unknown);
        return -EINVAL;
    }

    return check_lock_range(start, len);
}
