/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * x86_64 host-arch definitions of the Windows VBS-enclave PAL (dev docs: vbs-enclave/
 * PLAN-x64.md, X2). %gs is the Windows TEB of the enclave thread and is never written by packed
 * code; the PAL_TCB of a thread lives inside its TEB at PAL_TCB_GS_OFFSET (pal_tcb_gs.h), where the
 * vbs PAL places it (vbs_arch_place_tcb() in pal/src/host/vbs/pal_vbs.h). Setting the TCB is
 * therefore only a check that it is there.
 */

#pragma once

#ifdef IN_PAL

#include "api.h"
#include "cpu.h"
#include "pal_tcb_gs.h"

#define ARCH_HIGHEST_ADDR ((1ul << 47) - PAGE_SIZE)

static inline uintptr_t pal_vbs_teb(void) {
    uintptr_t teb;
    __asm__ volatile("rdgsbase %0" : "=r"(teb));
    return teb;
}

static inline int pal_set_tcb(PAL_TCB* tcb) {
    return (uintptr_t)tcb == pal_vbs_teb() + PAL_TCB_GS_OFFSET ? 0 : -PAL_ERROR_INVAL;
}

#endif /* IN_PAL */
