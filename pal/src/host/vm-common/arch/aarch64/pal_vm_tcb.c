/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 VM-PAL backend of the PAL_TCB lookup contract in pal_arch.h's pal_get_tcb().
 *
 * The Linux PAL (Milestone A) resolves the running thread's PAL_TCB from TPIDR_EL0 through a
 * tag bit and a full TLS->TCB registry, because PAL code there shares EL0 with the application
 * and TPIDR_EL0 belongs to the app. In the unikernel there is no such separation: the kernel
 * owns the "current TCB" (per_cpu_data->cur_tcb, see kernel_multicore.h) and libsysdb.so runs
 * at EL0, which cannot read TPIDR_EL1. So the kernel mirrors cur_tcb into the exported cell
 * `g_pal_tcb_singleton` below -- written by save_context_and_restore_next() right next to the
 * cur_tcb store and by set_dummy_gs_base() during boot/teardown (grep `g_pal_tcb_singleton` in
 * the aarch64 vm sources for the two maintenance sites). While a thread runs, the cell holds
 * its TCB: exactly the invariant x86_64 gets from a ring-3-readable %gs base.
 *
 * The registry entry points (called by LibOS's set_tls()/thread-exit paths on aarch64) are
 * no-ops here by the same argument: LibOS's per-application-thread TLS registration matters
 * only to the Linux-PAL lookup path. They are kept -- and exported, their names are already in
 * pal_symbols -- so LibOS links identically across hosts; scan() answers with the singleton,
 * matching "the cell holds the running thread's TCB". Multi-CPU boots (currently rejected by
 * init_multicore) would extend this to one cell per CPU, indexed by a value handed to EL0 at
 * syscall entry.
 */

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "pal.h"

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
    PAL_TCB* tcb = g_pal_tcb_singleton;
    if (!tcb)
        return false;
    *out_tcb = tcb;
    return true;
}
