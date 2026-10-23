/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * aarch64 VM-PAL backend of the PAL_TCB lookup contract in pal_arch.h's pal_get_tcb().
 *
 * The Linux PAL (Milestone A) resolves the running thread's PAL_TCB from TPIDR_EL0 through a
 * tag bit and a full TLS->TCB registry, because PAL code there shares EL0 with the application
 * and TPIDR_EL0 belongs to the app. In the unikernel there is no such separation: the kernel
 * owns the "current TCB" (per_cpu_data->cur_tcb, see kernel_multicore.h) and libsysdb.so runs
 * at EL0, which cannot read TPIDR_EL1. The EL0-readable cell is the cur_tcb slot itself,
 * addressed through TPIDRRO_EL0 -- an EL1-writable, EL0-read-only register that every CPU
 * gets preloaded with `&per_cpu_data[cpu].cur_tcb` at boot (vm_bootloader.S for the BSP and
 * every PSCI-brought-up AP; init_multicore_prepare() upgrades the BSP's to the allocated
 * array). save_context_and_restore_next() writes the slot on every switch, so `mrs` + `ldr`
 * at EL0 always names *this CPU's* running thread: the exact analog of x86_64 getting it
 * from a ring-3-readable %gs base, and per-CPU exact on any number of CPUs.
 *
 * (History: the first design kept a second, *global* mirror `g_pal_tcb_singleton` written
 * next to the cur_tcb store. With two CPUs that cell was arbitrary shared state -- an AP
 * parking its boot context stored NULL there while a fully-userspace thread on another CPU
 * read it, crashing LibOS at TCB-relative offset -0x48. The singleton definition below is
 * kept only as a link-compatible vestige for anything that still names the symbol.)
 *
 * The registry entry points (called by LibOS's set_tls()/thread-exit paths on aarch64) are
 * no-ops here by the same argument: LibOS's per-application-thread TLS registration matters
 * only to the Linux-PAL lookup path. They are kept -- and exported, their names are already in
 * pal_symbols -- so LibOS links identically across hosts; scan() answers with this CPU's
 * cell, matching "the cell holds the running thread's TCB".
 */

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "pal.h"

/* vestigial: no VM code reads or writes it any more (see the history comment) */
__attribute__((visibility("protected"))) PAL_TCB* g_pal_tcb_singleton;

void libos_tcb_registry_add(PAL_TCB* tcb, uintptr_t guest_tp) {
    __UNUSED(tcb);
    __UNUSED(guest_tp);
}

void libos_tcb_registry_del(uintptr_t guest_tp, PAL_TCB* expected_tcb) {
    __UNUSED(guest_tp);
    __UNUSED(expected_tcb);
}

bool libos_tcb_registry_scan(PAL_TCB** out_tcb) {
    /* same per-CPU cell pal_arch.h's pal_get_tcb() reads at EL0 */
    PAL_TCB** cell;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(cell));
    PAL_TCB* tcb = *cell;
    if (!tcb)
        return false;
    *out_tcb = tcb;
    return true;
}
