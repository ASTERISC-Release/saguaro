/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_interrupts.c.
 *
 * Dispatch model (mirrors x86, see that file for the canonical comments):
 *   - The el0_irq asm entry (kernel_context.S) snapshots all user GPRs + ELR/SPSR/SP_EL0 into
 *     `struct isr_regs` and calls `isr_c()` below; it is used for IRQs arriving while in EL0.
 *   - The el1_sync asm entry calls `isr_kernel_c()` for synchronous exceptions in the kernel
 *     itself (a Gramine kernel fault is fatal -- same policy as x86, which panics on #PF).
 *   - The el0_sync (`svc`) fast path never goes through this file: it is handled inline by the
 *     syscall dispatcher in kernel_context.S (x86 analog: `syscall_asm` in kernel_events.S).
 *
 * Synchronization notes are identical to the x86 file (per-interrupt-number discipline inside
 * the dispatcher).
 */

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_apic.h"
#include "kernel_interrupts.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_sched.h"
#include "kernel_time.h"
#include "kernel_virtio.h"

bool g_interrupts_enabled = false;

/* TLB-invalidation broadcast bookkeeping (single object, same design as x86; the IPI transport
 * is GIC SGI #1 instead of the custom vector-33 IPI). Multi-CPU is a Milestone-B follow-up --
 * on a single-CPU boot this is a plain local `tlbi`. */
static void tlb_invalidate_range(void* addr, size_t size) {
    uint64_t start = ALIGN_DOWN((uint64_t)addr, PAGE_SIZE);
    uint64_t end   = ALIGN_UP((uint64_t)addr + size, PAGE_SIZE);
    /* `tlbi vale1is` would take per-page addresses (needs the ASID); the unikernel uses one
     * address space with the kernel mapped in, so a broadcast all-EL1 invalidation is correct
     * and cheap: it runs once per page-table *batch*, not per fault. */
    (void)start;
    (void)end;
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/*
 * Dispatch of a Group-1 IRQ. `regs` points at the asm-saved frame (from either el0_irq_entry
 * or el1_irq_entry); on return the asm `eret`s to (possibly updated) ELR.
 * Note: unlike x86 isr_c(), the GIC acknowledge (ICC_IAR1_EL1) is done *here* in C, since the
 * asm entry must not consume IAR before reading this frame's registers. The matching EOI must
 * echo the INTID returned by IAR (GICv3 ICC_EOIR1_EL1 is write-with-INTID, not the x86-style
 * "end whatever is in-service" write).
 */
void isr_c(struct isr_regs* regs) {
    uint32_t intid = gic_acknowledge_intid();

    switch (intid) {
        case GIC_INTID_PPI_EL1_PHYS:
            /* kernel tick: same 100ms period as the x86 LAPIC timer */
            if (get_per_cpu_data()->cpu_id == 0) {
                /* only CPU0 handles timeouts and alarms */
                notify_about_timeouts_uninterruptable();
            }
            lapic_timer_rearm();
            lapic_signal_interrupt_complete(intid);

            /* timer IRQs always arrive from EL0 in this kernel (EL1 runs with DAIF.I masked
             * across scheduler critical sections); preempt unconditionally. */
            sched_thread_uninterruptable(regs);
            break;
        default:
            if (intid >= GIC_INTID_SPI_VIRTIO1 && intid < GIC_INTID_SPI_VIRTIO1 + 16) {
                /* virtio device IRQ: the x86 kernel polls all three virtio queues from the
                 * single SPI 64 line because all three devices shared one interrupt line;
                 * QEMU virt gives each device its own SPI, but the drivers are queue-atomic
                 * and tolerate being kicked for another device's IRQ, so keep the same fan-out
                 * until we learn each device's exact INTID during PCI enumeration. */
                if (virtio_console_isr() < 0 || virtio_fs_isr() < 0 || virtio_vsock_isr() < 0) {
                    log_error("Panic: virtio ISR failed (INTID %u)", intid);
                    triple_fault();
                }
                lapic_signal_interrupt_complete(intid);
                break;
            }
            if (intid == 1023) {
                /* spurious (already deactivated / race with EOI) */
                break;
            }
            log_error("Panic: unhandled IRQ INTID %u (elr=0x%lx sp=0x%lx x0=0x%lx)", intid,
                      regs->elr, regs->sp, regs->regs[0]);
            triple_fault();
    }
}

/* Synchronous exception taken *inside the kernel* (EL1). Faults in the unikernel kernel are
 * bugs; report the frame and stop (x86 analog: #PF/#GP panic paths). */
void isr_kernel_c(struct isr_regs* regs) {
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    log_error("Panic: EL1 sync exception ESR=0x%lx FAR=0x%lx", esr, far);
    log_error("       elr=0x%lx sp=0x%lx x0=0x%lx x30=0x%lx", regs->elr, regs->sp, regs->regs[0],
              regs->regs[30]);
    triple_fault();
}

int send_invalidate_tlb_ipi_and_wait(void* addr, size_t size, bool invalidate_on_this_cpu) {
    assert(IS_ALIGNED_PTR(addr, PAGE_SIZE) && IS_ALIGNED(size, PAGE_SIZE));

    if (g_num_cpus > 1) {
        /* TODO(Milestone-B multicore): SGI-based broadcast protocol (mirrors the x86
         * `num_responses` handshake, hardened against spurious SGIs). */
        log_error("TLB shootdown across CPUs is not implemented yet");
        return -PAL_ERROR_NOTIMPLEMENTED;
    }

    if (invalidate_on_this_cpu)
        tlb_invalidate_range(addr, size);
    return 0;
}

int interrupts_init(void) {
    /* The vector table itself is installed by syscalls_init() (VBAR_EL1) before we get here;
     * the timer and its interrupt line were configured by gic_init()/lapic_timer_init().
     * Unmask Group-1 IRQs at EL1 now that everything that must be ready is ready. */
    sti();
    g_interrupts_enabled = true;
    return 0;
}
