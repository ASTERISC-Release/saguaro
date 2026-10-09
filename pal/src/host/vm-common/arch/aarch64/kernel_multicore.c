/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_multicore.c.
 *
 * Architectural differences (see arch/aarch64/kernel_multicore.h for the TCB/per-CPU register
 * design):
 *   - Per-CPU data lives at TPIDR_EL1 (x86: GS_KERNEL_BASE); set once per CPU and never again.
 *   - Secondary CPUs are started with the PSCI `CPU_ON` SMC call (QEMU virt implements PSCI in
 *     firmware; there is no INIT-SIPI-SIPI trampoline and no 32K AP startup page). The AP entry
 *     point is the `pal_ap_entry` asm trampoline in kernel_context.S.
 *   - The current-thread-TSL slot (`cur_tcb`) replaces the swappable GS base; nothing to do here
 *     at boot beyond providing the early static per-CPU struct the bootloader hands over.
 */

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "cpu.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_host.h"
#include "pal_internal.h"

#include "kernel_dtb.h"
#include "kernel_interrupts.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_sched.h"
#include "kernel_time.h"
#include "kernel_xsave.h"

uint32_t g_num_cpus = 0;
struct per_cpu_data* g_per_cpu_data = NULL;
void* g_per_cpu_interrupt_stack;
void* g_per_cpu_interrupt_fpsimd_area;

static uint32_t g_started_cpus = 0; /* CPUs that started initialization; accessed atomically */
static uint32_t g_ready_cpus   = 0; /* CPUs that finished initialization; accessed atomically */

/*
 * The very early per-CPU struct, live in .data at link time: pal_start_c installs it into
 * TPIDR_EL1 before anything else runs (set_dummy_gs_base() and the first spinlocks need a
 * valid `get_per_cpu_data()`), and init_multicore_prepare() migrates its contents into the
 * real dynamically-allocated array below.
 */
struct per_cpu_data g_early_per_cpu_data = { .cpu_id = 0 };

long psci_call(uint64_t func_id, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    register uint64_t x0 __asm__("x0") = func_id;
    register uint64_t x1 __asm__("x1") = arg0;
    register uint64_t x2 __asm__("x2") = arg1;
    register uint64_t x3 __asm__("x3") = arg2;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return (long)x0;
}

/* asm trampoline in kernel_context.S; PSCI CPU_ON enters EL1 with the target address in x1 */
extern void pal_ap_entry(void);

int init_multicore_prepare(uint32_t num_cpus) {
    assert(num_cpus >= 1 && num_cpus <= MAX_NUM_CPUS);

    /* allocate all per-CPU arrays: one entry per CPU, each entry cache-line padded so that
     * per_cpu_data fields and their stacks don't false-share between CPUs */
    void* per_cpu_area = memory_get_shared_region(num_cpus * sizeof(struct per_cpu_data)
                                                  + 64);
    if (!per_cpu_area)
        return -PAL_ERROR_NOMEM;
    g_per_cpu_data = per_cpu_area;

    g_per_cpu_interrupt_stack = memory_get_shared_region(num_cpus * INTERRUPT_STACK_SIZE);
    if (!g_per_cpu_interrupt_stack)
        return -PAL_ERROR_NOMEM;

    g_per_cpu_interrupt_fpsimd_area = memory_get_shared_region(num_cpus
                                                               * INTERRUPT_XSAVE_AREA_SIZE);
    if (!g_per_cpu_interrupt_fpsimd_area)
        return -PAL_ERROR_NOMEM;

    void* scheduling_stacks = memory_get_shared_region(num_cpus * SCHEDULING_STACK_SIZE);
    if (!scheduling_stacks)
        return -PAL_ERROR_NOMEM;

    /* migrate the boot-time per-CPU state (cur_tcb installed by set_dummy_gs_base() during
     * pal_start_c, before any allocation was possible) into the real cpu0 slot */
    uint64_t early_cur_tcb = (uint64_t)g_early_per_cpu_data.cur_tcb;

    for (uint32_t i = 0; i < num_cpus; i++) {
        struct per_cpu_data* data = &g_per_cpu_data[i];
        data->cpu_id = i;
        data->interrupt_stack = (char*)g_per_cpu_interrupt_stack
                                + (i + 1) * INTERRUPT_STACK_SIZE; /* stack grows down */
        data->interrupt_fpsimd_area = (char*)g_per_cpu_interrupt_fpsimd_area
                                      + i * INTERRUPT_XSAVE_AREA_SIZE;
        data->scheduling_stack = (char*)scheduling_stacks + (i + 1) * SCHEDULING_STACK_SIZE;
        data->invalidate_tlb_ipi_received = 0;
        data->cur_tcb = NULL;
    }
    g_per_cpu_data[0].cur_tcb = (void*)early_cur_tcb;

    /* from now on TPIDR_EL1 points at the dynamic array */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&g_per_cpu_data[0]));

    g_num_cpus = num_cpus;
    return 0;
}

int init_multicore(uint32_t num_cpus) {
    int ret;

    assert(g_per_cpu_data);

    /* create per-CPU idle threads (x86 does the same here; sched_thread's find_next_thread()
     * asserts their existence before any user thread exists) */
    if (num_cpus > 1) {
        /* TODO(Milestone-B multicore): PSCI CPU_ON loop:
         *   psci_call(PSCI_FUNC_CPU_ON, target_mpidr, &pal_ap_entry, cpu_idx)
         * with g_started_cpus/g_ready_cpus handshake like x86. AP bring-up needs the per-AP
         * VBAR/stack/PAT-equivalents finished first; until then refuse multi-CPU boots loudly
         * instead of silently running one CPU.
         */
        log_error("aarch64 VM PAL supports a single CPU for now (QEMU: -smp 1)");
        return -PAL_ERROR_NOTIMPLEMENTED;
    }

    for (uint32_t i = 0; i < num_cpus; i++) {
        struct thread* thread;
        ret = thread_helper_create(thread_idle_run, &thread);
        if (ret < 0)
            return ret;
        g_per_cpu_data[i].idle_thread = thread;
    }

    if (num_cpus > 0) {
        /* only CPU0 has a bottomhalves thread currently */
        ret = thread_helper_create(thread_bottomhalves_run,
                                   &g_per_cpu_data[0].bottomhalves_thread);
        if (ret < 0)
            return ret;
    }

    __atomic_store_n(&g_started_cpus, num_cpus, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ready_cpus, num_cpus, __ATOMIC_RELEASE);
    return 0;
}

/*
 * C part of secondary-CPU bring-up; entered from the `pal_ap_entry` asm trampoline (which set
 * TPIDR_EL1 to this CPU's per_cpu_data slot and switched to the interrupt stack). Cooperative
 * scheduler takes over from the idle thread loop exactly like on CPU0.
 */
noreturn void pal_start_ap_c(uint32_t cpu_idx) {
    __UNUSED(cpu_idx);
    __UNUSED(g_ready_cpus);
    /* The idle thread is not yet created for APs (single-CPU policy above); the AP would spin
     * on a WFI loop here. PSCI CPU_STATE_OFF on return. */
    while (true) {
        __asm__ volatile("wfi");
    }
    __builtin_unreachable();
}
