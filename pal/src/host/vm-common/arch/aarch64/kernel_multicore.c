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

#include "kernel_apic.h"
#include "kernel_dtb.h"
#include "kernel_interrupts.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_sched.h"
#include "kernel_syscalls.h"
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

/*
 * PSCI smc/hvc conduit call (the arm equivalent of the x86 fw_cfg/mailbox bootstrap). The
 * conduit is firmware policy, not kernel policy: QEMU virt's DTB advertises `method = "hvc"`
 * for TCG direct-boot (UEFI-backed machines use smc), and calling the wrong one just traps
 * as an undefined instruction, so the DTB-parsed choice from dtb_init() drives it.
 *
 * The conduit must be decided *before* the register-argument pinning and the two conduits
 * must stay in separate noinline functions: an earlier revision branched on
 * dtb_psci_conduit_hvc() *between* the x0-x3 initializations and the asm, and GCC's
 * register-variable handling let that call's return value (1) survive in x0 at the hvc --
 * every call reached QEMU as function-id 1 and returned PSCI NOT_SUPPORTED (-1). Same law
 * as arm_smccc_smc() in Linux or any svc-adjacent asm: no C in the register window.
 */
static long __attribute__((noinline)) psci_call_hvc(uint64_t func_id, uint64_t arg0,
                                                    uint64_t arg1, uint64_t arg2) {
    register uint64_t x0 __asm__("x0") = func_id;
    register uint64_t x1 __asm__("x1") = arg0;
    register uint64_t x2 __asm__("x2") = arg1;
    register uint64_t x3 __asm__("x3") = arg2;
    __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return (long)x0;
}

static long __attribute__((noinline)) psci_call_smc(uint64_t func_id, uint64_t arg0,
                                                    uint64_t arg1, uint64_t arg2) {
    register uint64_t x0 __asm__("x0") = func_id;
    register uint64_t x1 __asm__("x1") = arg0;
    register uint64_t x2 __asm__("x2") = arg1;
    register uint64_t x3 __asm__("x3") = arg2;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return (long)x0;
}

long psci_call(uint64_t func_id, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    if (dtb_psci_conduit_hvc())
        return psci_call_hvc(func_id, arg0, arg1, arg2);
    return psci_call_smc(func_id, arg0, arg1, arg2);
}

#define PSCI_FUNC_SYSTEM_OFF 0x84000008ULL

/* Power the whole machine off (QEMU exits). Called from triple_fault(), i.e. from exit and
 * panic paths that may run before the DTB is parsed or re-enter via a fault in this very
 * call, so: refuse without a DTB-advertised PSCI, and only ever try once. */
void arm_psci_system_off(void) {
    static volatile int attempted = 0;
    if (!dtb_psci_available() || __atomic_exchange_n(&attempted, 1, __ATOMIC_ACQ_REL))
        return;
    psci_call(PSCI_FUNC_SYSTEM_OFF, 0, 0, 0);
}

/* PSCI v0.2/v1.0 64-bit function ids (arm DEN 0022A); the DTB's /psci properties carry the
 * same values -- dtb_cpu_on_id() reads *cpu ids* from cpu@N reg, this is the *function id*. */
#define PSCI_FUNC_CPU_ON 0xc4000003ULL

/* asm trampoline in vm/arch/aarch64/vm_bootloader.S: re-establishes the per-CPU sysregs
 * (TPIDR_EL1 stack/tables/VBAR) with the MMU off and calls pal_start_ap_c(x0 = cpu idx) */
extern void pal_ap_entry(void);

/* Serializes AP C bring-up exactly like x86's g_lock_single_ap_cpu: each AP's boot context
 * parks in sched_thread(), which releases this lock on its way out of the CPU, admitting the
 * next AP. Nothing in today's AP bring-up strictly needs it (every step is per-CPU sysregs
 * or idempotent globals), but keeping the same one-at-a-time ordering as x86 bounds the
 * blast radius of any future BSP-shared work in the AP path. */
static uint32_t g_lock_single_ap_cpu = 0;

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

    /* create per-CPU idle threads + CPU0's bottomhalves thread here (x86 creates them in
     * init_multicore_prepare too): the LAPIC/GIC timer is already armed by gic_init() and the
     * tick may be taken by el1_irq_entry the moment interrupts_init() unmasks DAIF, long before
     * init_multicore() runs. find_next_thread() then needs a non-NULL idle/bottomhalves fallback
     * (assert() is a no-op in this build, a NULL there otherwise reaches
     * save_context_and_restore_next as next_tcb == (void*)-280 and branches to PC 0). */
    int ret;
    for (uint32_t i = 0; i < num_cpus; i++) {
        struct thread* thread;
        ret = thread_helper_create(thread_idle_run, &thread);
        if (ret < 0)
            return ret;
        g_per_cpu_data[i].idle_thread = thread;
    }

    /* only CPU0 has a bottomhalves thread currently (i.e. CPU0 handles all incoming events) */
    ret = thread_helper_create(thread_bottomhalves_run, &g_per_cpu_data[0].bottomhalves_thread);
    if (ret < 0)
        return ret;

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
    /* keep this CPU's EL0-readable TCB cell pointing at the upgraded slot (installed in
     * vm_bootloader.S at the early static instance; APs install theirs in pal_ap_entry) */
    __asm__ volatile("msr tpidrro_el0, %0" :: "r"(&g_per_cpu_data[0].cur_tcb));

    g_num_cpus = num_cpus;
    return 0;
}

int init_multicore(uint32_t num_cpus) {
    assert(g_per_cpu_data);
    assert(num_cpus >= 1 && num_cpus <= MAX_NUM_CPUS);

    /* The BSP counts itself in both counters (it finished its own bring-up — including
     * interrupts_init() — before this point); x86's AP trampoline asm gets the started-count
     * in the same "each running CPU counts itself" way. Each AP adds itself in
     * pal_start_ap_c() below, so both counters must reach exactly num_cpus. */
    __atomic_store_n(&g_started_cpus, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ready_cpus, 1, __ATOMIC_RELEASE);

    if (num_cpus == 1)
        return 0;

    if (!dtb_psci_available()) {
        log_error("device tree describes %u CPUs but exposes no /psci node: cannot start "
                  "secondary CPUs (QEMU virt needs its PSCI firmware)", num_cpus);
        return -PAL_ERROR_NOTIMPLEMENTED;
    }

    /* CPU_ON args (arm DEN 0022A §5.3): target cpu id = the mpidr-affinity value the DTB
     * carries in cpu@N's reg, entry = pal_ap_entry (identity-mapped, so link address == PA
     * even with the AP's MMU off), context id = our cpu index — the PSCI spec boots the CPU
     * with x0 = context id, which is how pal_ap_entry learns which slot is its own. */
    /* The APs' trampoline installs g_page_table_root into their TTBR0 (see vm_bootloader.S):
     * the fine tables must exist before any CPU_ON, i.e. memory_pagetables_init() (early in
     * pal_start_c) must have run. It always has -- this assert documents the ordering for
     * anyone who ever moves init_multicore() earlier or makes the switch conditional. */
    extern uint64_t* g_page_table_root;
    assert(g_page_table_root);

    for (uint32_t i = 1; i < num_cpus; i++) {
        long ret = psci_call(PSCI_FUNC_CPU_ON, dtb_cpu_on_id(i),
                             (uint64_t)(uintptr_t)&pal_ap_entry, /*context id=*/i);
        if (ret != 0) {
            log_error("PSCI CPU_ON for CPU %u (cpu id 0x%x) failed: PSCI error %ld",
                      i, dtb_cpu_on_id(i), ret);
            return -PAL_ERROR_DENIED;
        }
    }

    /* The same two-stage handshake as x86, and for the same reason: `started` proves each AP
     * reached C (a silent PSCI no-op is distinguishable from a stuck trampoline), `ready`
     * proves every per-CPU GIC/timer/VBAR bring-up finished before the BSP assumes SMP.
     * delay() runs with the BSP's IRQs live, so the BSP may be tick-preempted here — the
     * cooperative scheduler handles that (this loop holds no lock). */
    size_t tries = 0;
    while (__atomic_load_n(&g_started_cpus, __ATOMIC_ACQUIRE) != num_cpus) {
        if (tries++ > num_cpus * 5) {
            log_error("Waited for %u CPUs to start initialization, but timed out (%u started)",
                      num_cpus, __atomic_load_n(&g_started_cpus, __ATOMIC_RELAXED));
            return -PAL_ERROR_DENIED;
        }
        int ret = delay(10000, /*continue_gate=*/NULL);
        if (ret < 0)
            return ret;
    }

    tries = 0;
    while (__atomic_load_n(&g_ready_cpus, __ATOMIC_ACQUIRE) != num_cpus) {
        if (tries++ > num_cpus * 5) {
            log_error("Waited for %u CPUs to finish initialization, but timed out (%u ready)",
                      num_cpus, __atomic_load_n(&g_ready_cpus, __ATOMIC_RELAXED));
            return -PAL_ERROR_DENIED;
        }
        int ret = delay(10000, /*continue_gate=*/NULL);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/*
 * C part of secondary-CPU bring-up; entered from the `pal_ap_entry` asm trampoline (which set
 * TPIDR_EL1 to this CPU's per_cpu_data slot, switched to the interrupt stack and installed
 * VBAR). The sequence mirrors x86's pal_start_ap_c: enable this CPU's interrupt delivery,
 * arm its private timer, (re)install per-CPU exception entry, unmask, count ready — and then
 * park the boot context into the cooperative scheduler with sched_thread(), after which this
 * CPU is driven by its per-CPU idle/tick machinery exactly like CPU0.
 */
noreturn void pal_start_ap_c(uint32_t cpu_idx) {
    __atomic_add_fetch(&g_started_cpus, 1, __ATOMIC_RELEASE);

    assert(g_per_cpu_data && cpu_idx >= 1 && cpu_idx < MAX_NUM_CPUS);
    /* the trampoline installed TPIDR_EL1 = &g_per_cpu_data[cpu_idx] before calling us */
    assert(get_per_cpu_data() == &g_per_cpu_data[cpu_idx]);

    /* Group-1 delivery for this CPU: ICC_PMR_EL1 + ICC_IGRPEN1_EL1 are sysregs, per-CPU. */
    lapic_enable();

    /* Wake this CPU's redistributor frame, enable its PPI30 timer line and arm its own EL1
     * physical timer (100ms tick): every step addresses either a per-CPU sysreg or this
     * CPU's own redistributor, so APs can run it concurrently with each other and the BSP.
     * (Same no-check style as x86's pal_start_ap_c: none of these can fail after gic_init()
     * succeeded on the BSP, and a failed check would have no recovery path on an AP.) */
    lapic_timer_init();

    syscalls_init();

    /* sti() + g_interrupts_enabled (idempotent global, x86 interrupts_init does the same
     * per-AP): from here on this CPU takes ticks and the scheduler owns its time. */
    interrupts_init();

    __atomic_add_fetch(&g_ready_cpus, 1, __ATOMIC_RELEASE);

    /* Park the boot context and release the single-AP startup lock: sched_thread() saves this
     * context onto the per-CPU scheduling stack (curr_tcb == NULL is exactly the case the asm
     * was written for — there is no struct thread to save), switches this CPU to its idle
     * thread, and the AP-boot context is never resumed — its job was only to make the CPU
     * exist, like x86's. */
    sched_thread(&g_lock_single_ap_cpu, /*clear_child_tid=*/NULL);

    __builtin_unreachable();
}
