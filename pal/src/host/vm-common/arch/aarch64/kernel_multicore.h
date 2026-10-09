/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_multicore.h.
 *
 * x86_64 keeps two levels of "current" pointers in swap-able MSRs: GS_BASE points at the
 * current thread's Gramine TCB (swapped by save_context_and_restore_next on every kernel
 * context switch) and GS_KERNEL_BASE points at per-CPU data (set once per CPU). aarch64 has
 * exactly one EL1-writable software thread register, TPIDR_EL1, so it goes to the per-CPU data
 * (the GS_KERNEL_BASE role -- set once when a CPU comes up and never touched again). The
 * GS_BASE role (current thread TCB) becomes a plain field, `cur_tcb`, inside the per-CPU data:
 * reading "my TCB" is `mrs tpidr_el1` plus one load, which is exactly the x86 rdmsr+gs: chain
 * with one extra indirection, and it is per-CPU by construction (unlike GS_BASE it can never be
 * raced by another CPU's context switch).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_sched.h"
#include "kernel_thread.h"

#define SCHEDULING_STACK_SIZE    128     /* temp stack used by save_context_and_restore_next() */

/* aarch64 secondary CPUs are started by firmware (PSCI `CPU_ON` SMC call), not by an INIT-SIPI
 * triple with a trampoline page at 32K; there is no equivalent of the x86 AP startup page or of
 * the TDX MADT wakeup mailbox. */

struct per_cpu_data {
    uint32_t cpu_id;               /* 0 .. num_cpus-1 */
    uint32_t mpidr_low8_unused;    /* MPIDR_EL1[7:0] of this CPU (QEMU virt: == cpu_id) */
    void*    interrupt_stack;      /* start address of the stack used for interrupts */
    void*    interrupt_fpsimd_area;/* FP/SIMD save area used by the interrupt handler */

    void* scheduling_stack;        /* temporary stack used in save_context_and_restore_next() */

    struct thread* idle_thread;         /* each CPU has its own idle thread */
    struct thread* bottomhalves_thread; /* only CPU0 has a bottomhalves thread currently */

    uint8_t invalidate_tlb_ipi_received; /* to protect against extra "invalidate TLB" interrupts */
    uint8_t reserved[7];

    /* GS_BASE role: TCB (pal_tcb_vm) of the Gramine thread running on this CPU, or 0 while
     * executing kernel/boot code with no thread context. Maintained by
     * save_context_and_restore_next() and by the EL0 sync entry path, read by the `svc`
     * dispatcher to find the current thread's PAL TCB. `void*` because struct pal_tcb_vm is
     * defined in pal_common.h, which is above this header's include layer; all consumers
     * immediately pass it through get_thread_ptr()/replace_with_null_if_dummy_gs_base(). */
    void* cur_tcb;
} __attribute__((packed));

extern uint32_t g_num_cpus;

extern struct per_cpu_data* g_per_cpu_data;
extern void* g_per_cpu_interrupt_stack;
extern void* g_per_cpu_interrupt_fpsimd_area;

static inline struct per_cpu_data* get_per_cpu_data(void) {
    uint64_t per_cpu_data_ptr;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(per_cpu_data_ptr));
    return (struct per_cpu_data*)per_cpu_data_ptr;
}

/* the four gsbase-helper functions (declared in kernel_sched.h) are implemented in
 * arch/aarch64/pal_common_threading.c using cur_tcb above */

noreturn void pal_start_ap_c(uint32_t cpu_idx);

int init_multicore_prepare(uint32_t num_cpus);
int init_multicore(uint32_t num_cpus);

/* PSCI interface (SMC64 calls to QEMU's in-kernel PSCI implementation): used to power on
 * secondary CPUs and to shut the VM down. */
#define PSCI_FUNC_CPU_ON     0xC4000003u
#define PSCI_FUNC_SYSTEM_OFF 0x84000008u

long psci_call(uint64_t func_id, uint64_t arg0, uint64_t arg1, uint64_t arg2);
