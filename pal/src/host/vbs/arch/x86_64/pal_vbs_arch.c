/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * x86_64 parts of the vbs PAL proper (Documentation/devel/vbs-enclave/PLAN-x64.md).
 *
 * The thread pointer of the Linux code is the FS base (FSGSBASE works in a VBS enclave). The kernel
 * does not keep it reliably for enclave threads -- preemption resets it to 0, an OCALL can leave a
 * stale value (FINDINGS-x64.md) -- so the value is also kept in the thread's TEB, in
 * NT_TIB.ArbitraryUserPointer (gs:0x28): the Windows enclave runtime writes it back after every
 * service call and when a fault shows it lost (pal_vbs_boot.c). GS is the TEB and is never set.
 *
 * CPUID works natively in the enclave and returns the host's values (FINDINGS-x64.md).
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

#define TEB_FS_SHADOW 0x28 /* NT_TIB.ArbitraryUserPointer */

int _PalSegmentBaseGet(enum pal_segment_reg reg, uintptr_t* addr) {
    switch (reg) {
        case PAL_SEGMENT_FS: {
            uintptr_t v;
            __asm__ volatile("movq %%gs:%c1, %0" : "=r"(v) : "i"(TEB_FS_SHADOW));
            *addr = v;
            return 0;
        }
        case PAL_SEGMENT_GS:
            /* the TEB of the enclave thread belongs to Windows: no GS base for applications */
            *addr = 0;
            return 0;
        default:
            return -PAL_ERROR_INVAL;
    }
}

int _PalSegmentBaseSet(enum pal_segment_reg reg, uintptr_t addr) {
    switch (reg) {
        case PAL_SEGMENT_FS:
            if (addr >= (1ul << 47))
                return -PAL_ERROR_INVAL; /* not canonical (wrfsbase would fault) */
            __asm__ volatile("movq %0, %%gs:%c1\n"
                             "wrfsbase %0\n"
                             : : "r"(addr), "i"(TEB_FS_SHADOW) : "memory");
            return 0;
        case PAL_SEGMENT_GS:
            return -PAL_ERROR_NOTSUPPORT;
        default:
            return -PAL_ERROR_INVAL;
    }
}

int _PalCpuIdRetrieve(uint32_t leaf, uint32_t subleaf, uint32_t values[4]) {
    __asm__ volatile("cpuid"
                     : "=a"(values[0]), "=b"(values[1]), "=c"(values[2]), "=d"(values[3])
                     : "a"(leaf), "c"(subleaf));
    return 0;
}

/* the first thread's TCB in its TEB (pal_main.c, pal_vbs.h) */
PAL_VBS_TCB* g_vbs_first_tcb;
