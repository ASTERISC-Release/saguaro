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

/* TLB-invalidation bookkeeping for the software shootdown protocol below.
 *
 * The architecture's inner-shareable TLBI (`tlbi vmalle1is` + `dsb ish`) would be sufficient
 * on real hardware -- but QEMU's TCG implements every TLBI, shareable flavors included, as a
 * flush of the *executing vCPU's* TLB only (target/arm/tcg/tlb-insns.c: tlbi_aa64_vmalle1is
 * calls the local tlb_flush; there is no cross-vCPU shootdown anywhere in the emulator). A
 * guest that trusts the broadcast therefore keeps stale -- or, after a block-split race is
 * fixed, simply *missing-permission* -- entries on every other core; Milestone-B SMP proved
 * exactly this by losing freshly mmap'd thread-stack pages on non-mapping CPUs. So the arm
 * port runs the same `num_responses` handshake the x86 port assembles for INVLPG, with GIC
 * SGI #1 as the IPI transport; on real hardware the extra per-CPU flushes are redundant but
 * harmless (flushing twice never leaves a stale entry). */
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

/* Shootdown protocol state; same fields, same locking discipline as the x86
 * `g_invalidate_tlb_request` (vm-common/kernel_interrupts.c). Declared before isr_c because
 * the SGI receive case runs this handshake from interrupt context. */
static spinlock_t g_invalidate_tlb_request_lock = INIT_SPINLOCK_UNLOCKED;
static struct {
    volatile bool in_progress;
    volatile void* addr;
    volatile size_t size;
    volatile uint32_t num_responses;
} g_invalidate_tlb_request;

/* TODO(diag SMP B7): per-CPU SGI handler branch history (drop with the hang debugging) */
volatile uint32_t g_dbg_sgi_branch[8 * 3]; /* per cpu: extra, spurious, responded */

/* TODO(diag SMP B7): per-CPU tick liveness: if a stalled run's non-responder stops counting
 * ticks, that CPU is stuck with IRQs masked (not an SGI delivery problem). */
volatile uint32_t g_dbg_ticks[8];

/* TODO(diag SMP B7): event ring per CPU: low 10 bits = INTID, bit 10 = 1 for EOI (vs IAR).
 * An IAR with no following EOI entry = CPU-interface active-priority wedge; an SGI IAR missing
 * entirely = masked/deaf CPU. */
volatile uint32_t g_dbg_events[8][16];
volatile uint32_t g_dbg_event_idx[8];

/* TODO(diag SMP B7): the shared kernel_time.c cannot include the aarch64
 * kernel_multicore.h (include order resolves it to the x86 implementation), so expose the
 * current cpu id through this helper for its live-DAIF probe. */
uint32_t dbg_this_cpu_id(void) {
    return get_per_cpu_data()->cpu_id;
}

static void dbg_record_event(uint32_t intid, int is_eoi) {    uint32_t cpu = get_per_cpu_data()->cpu_id;
    if (cpu >= 8)
        return;
    uint32_t idx = g_dbg_event_idx[cpu]++ & 15;
    g_dbg_events[cpu][idx] = intid | (is_eoi ? 0x400 : 0);
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
    dbg_record_event(intid, /*is_eoi=*/0); /* TODO(diag SMP B7) */

    switch (intid) {
        case GIC_INTID_PPI_EL1_PHYS:
            /* kernel tick: same 100ms period as the x86 LAPIC timer */
            if (get_per_cpu_data()->cpu_id < 8) {
                uint32_t tc = ++g_dbg_ticks[get_per_cpu_data()->cpu_id];
                /* TODO(diag SMP B7): tick liveness heartbeat (also fires on the EL1-int
                 * interrupted path: the tick gate is after this). */
                if (tc % 10 == 0)
                    log_always("DBG: tick cpu%u n=%u",
                               (unsigned)get_per_cpu_data()->cpu_id, tc);
            }
            if (get_per_cpu_data()->cpu_id == 0) {
                /* only CPU0 handles timeouts and alarms */
                notify_about_timeouts_uninterruptable();
            }
            lapic_timer_rearm();
            lapic_signal_interrupt_complete(intid);

            /* x86 gate `regs->cs != kernel_cs` verbatim: preemptive userland scheduling but
             * COOPERATIVE kernel scheduling. The comment this replaces ("timer IRQs always
             * arrive from EL0") was wrong: save_context_and_restore_next unmasks IRQs mid-way
             * (msr daif, popfq parity) and then lives with live user x19-x30 in registers
             * while it branches into isr_eret_to_userland or returns into sched_thread C --
             * an identical window to x86's popfq-then-movq tail. A tick landing there took
             * the el1_irq frame, and sched_thread_uninterruptable() happily re-saved it as
             * the *userland* context of the thread being restored (save_userland_context):
             * kernel asm PC into irq_pseudo_stack.rip, kernel SP into .rsp, SPSR = EL1h, and
             * half-reloaded user x19-x30 into context.regs. The thread's next dispatch then
             * `eret`ed into mid-kernel-asm with kernel register values (SPSR.EL gate sends
             * the eret to EL1, not EL0), where its later syscalls carried kernel addresses
             * as args (observed: futex op = FUTEX_WAKE_PRIVATE | garbage, loads from PAL bss
             * by app code). On one CPU the poison was almost always laundered away by the
             * resume path re-restoring the same thread before it ever reached EL0; SMP made
             * the migration-with-poisoned-context common. */
            if ((regs->spsr & 0xc) == 0 /* SPSR.M[3:2] == 0b00: interrupted EL0t */) {
                sched_thread_uninterruptable(regs);
            }
            break;
        case GIC_INTID_TLB_SHOOTDOWN_SGI: {
            /* TODO(diag SMP B7): branch counters for the shootdown hang (drop later) */
            uint32_t cpu = get_per_cpu_data()->cpu_id;
            /* TODO(diag SMP B7): CPU-interface ground truth from this still-running CPU while
             * shootdown rounds hang: ICC_HPPIR1_EL1 is what a future IAR1 would return (under
             * TCG: QEMU's model state; under KVM: the hardware vGIC answers natively, no trap).
             * hppi==30 on a tick-frozen CPU => the cpu interface owes that vCPU an undelivered
             * INTID 30 (delivery-side wedge); hppi==1023 => the GICR_ISPENDR0 bit30 seen in the
             * stall dump never propagates redist->cpuif. `spsr` also reports the interrupted
             * context's EL/mask (last probe that observes the *live* PSTATE on that vCPU). */
            {
                uint64_t hp = 1023, pmr = 0;
                __asm__ volatile("mrs %0, icc_hppir1_el1" : "=r"(hp));
                __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(pmr));
                static volatile uint32_t dbg_sgi_probe_logged;
                if (cpu < 8 && g_dbg_ticks[cpu] >= 2 && dbg_sgi_probe_logged < 64) {
                    dbg_sgi_probe_logged++;
                    log_always("DBG: cpu%u SGI spsr=%08lx elr=%016lx hppi=%lu pmr=%02lx ticks=%u",
                               cpu, (unsigned long)regs->spsr, (unsigned long)regs->elr,
                               (unsigned long)(hp & 0x3ff), (unsigned long)(pmr & 0xff),
                               g_dbg_ticks[cpu]);
                }
            }
            /* "invalidate TLB" IPI -- may be spurious/extra, check for this (the hardening
             * mirrors x86's `case 33` verbatim, down to the malicious-host comments). Unlike
             * the tick this handler NEVER reschedules -- it is the ack that releases the
             * waiting CPU, and descheduling here could strand the protocol behind an idle
             * loop. Unlike x86 the EOI must run even for a dropped SGI: GICv3 keeps an
             * acknowledged INTID ACTIVE until ICC_EOIR1_EL1 echoes it, so skipping the EOI
             * would permanently wedge future shootdown IPIs. */
            if (get_per_cpu_data()->invalidate_tlb_ipi_received) {
                /* ignore extra IPI (most probably there is a malicious host) */
                if (cpu < 8)
                    g_dbg_sgi_branch[cpu * 3 + 0]++;
                log_always("DBG: cpu%u SGI EXTRA (flag still 1, resp=%u ip=%d)", cpu,
                           __atomic_load_n(&g_invalidate_tlb_request.num_responses,
                                           __ATOMIC_RELAXED),
                           (int)__atomic_load_n(&g_invalidate_tlb_request.in_progress,
                                                __ATOMIC_RELAXED));
            } else if (!__atomic_load_n(&g_invalidate_tlb_request.in_progress,
                                        __ATOMIC_ACQUIRE)) {
                /* ignore spurious IPI (most probably there is a malicious host) */
                if (cpu < 8)
                    g_dbg_sgi_branch[cpu * 3 + 1]++;
                log_always("DBG: cpu%u SGI SPURIOUS (in_progress=0)", cpu);
            } else if (!__atomic_load_n(&g_invalidate_tlb_request.num_responses,
                                        __ATOMIC_ACQUIRE)) {
                /* now this is weird -- some vCPU started the protocol but num of responses is 0 */
                log_error("Panic: `invalidate TLB` IPI received, but state is inconsistent");
                triple_fault();
            } else {
                /* Drop this CPU's entries. x86 invlpg()'s the request's page range; we flush
                 * all EL1 entries (the same operation the sender performs locally --
                 * tlb_invalidate_range ignores the range and flushes everything, see its
                 * comment; a *narrower* receiver flush against a full sender flush would be
                 * the actual bug). */
                if (cpu < 8)
                    g_dbg_sgi_branch[cpu * 3 + 2]++;
                tlb_invalidate_range(NULL, 0);
                get_per_cpu_data()->invalidate_tlb_ipi_received = 1;
                __atomic_fetch_add(&g_invalidate_tlb_request.num_responses, 1,
                                   __ATOMIC_ACQ_REL);
            }
            lapic_signal_interrupt_complete(intid);
            break;
        }
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

/* EL0 synchronous exception that is not an `svc` (see the EC filter in el0_sync_entry). The
 * x86 analog is the #PF (cr2) / #GP IDT handlers: the frame is forwarded through
 * pal_common_perform_memfault_handling, so LibOS's VMA layer either fixes the mapping in
 * place or rewrites the frame to run the app's own SIGSEGV/SIGBUS handler; the asm then
 * resumes EL0 from the rewritten frame.
 *
 * EC decode (ARMv8 D1.4.4): because this path is the *EL0* synchronous vector, every fault here
 * came from EL0 ("lower EL" relative to the EL1 handler), so the abort ECs are ESR_ELx_EC_IABT_LOW
 * = 0x20 (instruction abort from EL0) and ESR_ELx_EC_DABT_LOW = 0x24 (data abort from EL0); the
 * 0x21/0x25 "same/current-EL" twins belong to the EL1 vector (isr_kernel_c) and are not seen
 * here. FAR_EL1 is the analog of cr2. The DFSC then says why: 0b0001LL translation fault (level
 * LL), 0b0010LL access-flag, 0b0011LL permission fault -- e.g. a write to EL0's NULL hits the
 * identity-mapped device window at VA 0, which is EL1-only, so EL0 gets a level-2 permission
 * fault (DFSC 0x0e), not a translation fault. Other ECs (notably EC=0x00 `udf`, whose x86 #UD
 * twin the x86 vm-PAL likewise does not forward) stay a visible panic; SIGILL for those is a
 * B-tail item. Entered with `regs` the fully populated el0_irq-shaped frame on the interrupt
 * stack; only x16's slot holds the EC-decode scratch value rather than the app's x16 (the
 * pre-dispatch `mrs/ubfx/cmp` has no other scratch GPR) -- harmless for C code, where x16 never
 * lives across an instruction. */
long el0_sync_unexpected_c(struct isr_regs* regs) {
    uint64_t esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uint64_t ec = esr >> 26;

    if (ec == 0x20 /* IABT from EL0 */ || ec == 0x24 /* DABT from EL0 */) {
        /* TODO(diag SMP B7): full app-register dump (drop after the clone-child pointer
         * corruption is root-caused): the app's x1/x2/x5 hold kernel-code addresses (PAL asm
         * PCs inside el1_sync_entry/dtb_init) when glibc heap-atomics fault, so pin down
         * which slot carries the kernel pointer and where it came from (x30 = caller). */
        if (far >= 0x40080000 && far < 0x44329000) {
            uintptr_t gs = replace_with_null_if_dummy_gs_base(
                (uintptr_t)get_per_cpu_data()->cur_tcb);
            struct thread* t = gs ? get_thread_ptr(gs) : NULL;
            log_always("DBG: EL0 fault far=0x%lx elr=0x%lx cpu=%u thr=%p sp=0x%lx",
                       far, regs->elr, get_per_cpu_data()->cpu_id, (void*)t, regs->sp);
            log_always("DBG:  x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx x4=0x%lx x5=0x%lx x8=0x%lx",
                       regs->regs[0], regs->regs[1], regs->regs[2], regs->regs[3],
                       regs->regs[4], regs->regs[5], regs->regs[8]);
            log_always("DBG:  x19=0x%lx x20=0x%lx x21=0x%lx x24=0x%lx x29=0x%lx x30=0x%lx",
                       regs->regs[19], regs->regs[20], regs->regs[21], regs->regs[24],
                       regs->regs[29], regs->regs[30]);
            /* kernel-side context image vs the live frame: if the tcb's regs hold the same
             * kernel values as the fault frame, the poison was in the TCB and the preemptive
             * resume loaded it; if the tcb holds sane user values, the poison appeared after
             * the eret. */
            if (t) {
                log_always("DBG: tcb ctx r19=0x%lx r20=0x%lx r21=0x%lx r24=0x%lx r29=0x%lx",
                           t->context.regs[19], t->context.regs[20], t->context.regs[21],
                           t->context.regs[24], t->context.regs[29]);
                log_always("DBG: tcb ctx pc=0x%lx sp=0x%lx pstate=0x%lx tls=0x%lx urip=0x%lx",
                           t->context.pc, t->context.sp, t->context.pstate,
                           t->context.user_tls, t->context.user_rip);
            }
        }
        if (pal_common_perform_memfault_handling(far, regs) == 0) {
            return 0; /* LibOS fixed the fault or delivered a signal from the frame */
        }
    }

    log_error("Panic: unhandled EL0 synchronous exception ESR=0x%lx (EC=0x%lx) ELR=0x%lx"
              " FAR=0x%lx", esr, ec, regs->elr, far);
    /* ESR_EL1 detail bits for EC=0x20..0x25 (faults): DFSC/IFSC [5:0], WnR [6], S1PTW [11].
     * FSC decodes as: 0b0001LL translation fault level LL, 0b0010LL access-flag level LL,
     * 0b0011LL permission fault level LL (QEMU's get_S1prot rules, see kernel_memory.c). */
    log_error("       FSC=0x%02lx %s WnR=%lu S1PTW=%lu IL=%lu", esr & 0x3f,
              (esr & (1UL << 11)) ? "S1PTW!" : "    ", (esr >> 6) & 1, (esr >> 11) & 1,
              (esr >> 31) & 1);
    log_error("       pc=0x%016lx sp=0x%016lx", regs->elr, regs->sp);
    for (unsigned i = 0; i < 32; i += 4) {
        if (i == 16) {
            log_error("       x16=(scratch) x17=0x%016lx x18=0x%016lx x19=0x%016lx",
                      regs->regs[17], regs->regs[18], regs->regs[19]);
            continue;
        }
        if (i == 28) {
            /* regs[] has 31 slots (x0..x30); show the frame's user SP in the last column */
            log_error("       x%02lu=0x%016lx x%02lu=0x%016lx x%02lu=0x%016lx sp =0x%016lx",
                      i + 0UL, regs->regs[i + 0], i + 1UL, regs->regs[i + 1],
                      i + 2UL, regs->regs[i + 2], regs->sp);
            break;
        }
        log_error("       x%02lu=0x%016lx x%02lu=0x%016lx x%02lu=0x%016lx x%02lu=0x%016lx",
                  i + 0UL, regs->regs[i + 0], i + 1UL, regs->regs[i + 1],
                  i + 2UL, regs->regs[i + 2], i + 3UL, regs->regs[i + 3]);
    }
    /* The faulting instruction itself, so the panic is analyzable from the log alone (the app
     * is ASLR'd, but its 32-bit encoding matches the host-side disassembly verbatim). Guarded
     * by a page-table lookup: ELR itself can be unmapped (prefetch abort), and EL1 has no
     * fixup table yet -- a fault here would triple-fault with an even less useful report. The
     * neighbors are only read when they share ELR's mapped page (an earlier version faulted on
     * insn[-1] at 0xfffffffffffffffc while dumping a jump-to-NULL; reading off the end of the
     * mapped page is exactly the unguarded read the guard exists to prevent). */
    uint64_t* pte;
    if (memory_find_page_table_entry(regs->elr, &pte) == 0 && (regs->elr & 0xfff) >= 8
        && (regs->elr & 0xfff) <= 0xff4) {
        const uint32_t* insn = (const uint32_t*)(uintptr_t)regs->elr;
        log_error("       insn @ELR: 0x%08x (prev 0x%08x, next 0x%08x)", insn[0], insn[-1],
                  insn[1]);
    } else if (memory_find_page_table_entry(regs->elr, &pte) == 0) {
        log_error("       insn @ELR: 0x%08x", *(const uint32_t*)(uintptr_t)regs->elr);
    }
    triple_fault();
    return -1;
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

    int ret;

    /* step 1: drop this CPU's own entries (IS flavor: a superset of local on real hardware,
     * exactly local under QEMU; the tlb_flush_all() callers pass addr=NULL/size=0, i.e. the
     * "flush everything" convention the full-EL1 flush implements regardless of range) */
    if (invalidate_on_this_cpu)
        tlb_invalidate_range(addr, size);

    if (!g_interrupts_enabled) {
        /* boot path: this CPU is the only one that can run yet (x86 comment verbatim: the
         * function may be called from bootstrap code before interrupts are truly enabled) */
        return 0;
    }

    spinlock_lock(&g_invalidate_tlb_request_lock);

    if (g_invalidate_tlb_request.in_progress || g_invalidate_tlb_request.num_responses) {
        /* sanity check that a previous "invalidate TLB" protocol run is finished */
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    for (uint32_t i = 0; i < g_num_cpus; i++) {
        /* reset the "IPI received" flag on each vCPU */
        g_per_cpu_data[i].invalidate_tlb_ipi_received = 0;
    }

    g_invalidate_tlb_request.in_progress = true;
    g_invalidate_tlb_request.addr = addr;
    g_invalidate_tlb_request.size = size;
    g_invalidate_tlb_request.num_responses = 1; /* this vCPU already invalidated */

    /* ICC_SGI1R_EL1 (GICv3 original-spec layout, which QEMU's icc_generate_sgi decodes):
     * INTID [27:24], IRM [40] = interrupt-routing-mode "all CPUs excluding self" (the
     * TargetList/Affinity fields are then ignored), TargetList [15:0]. Delivered as
     * non-secure Group-1 -- the group our ICC_IAR1/EOI1 pair consumes -- by QEMU's
     * icc_sgi1r_write. SGI #1 was enabled + grouped G1NS on every CPU's redistributor in
     * lapic_timer_init(). */
    uint64_t sgi = (1ULL << 40) | ((uint64_t)GIC_INTID_TLB_SHOOTDOWN_SGI << 24);
    /* GICv3 architecture (IHI 0069 "An Introduction to... GICv3", the SGI-generation
     * sequence): software must execute a DSB before writing ICC_SGI1R_EL1 so that the
     * data structures the receivers read are complete when their SGI arrives. The x86
     * twin of this function silently relies on TSO (plain stores globally visible in
     * order); on weakly-ordered aarch64 the receivers' vCPU threads can otherwise run
     * isr_c before the sender's in_progress=true/num_responses=1 stores become visible --
     * they hit the "ignore spurious IPI" branch, never respond, and the sender's wait
     * loop hangs (observed: stable resp=2/4 and 3/4 states with the silent CPUs'
     * invalidate_tlb_ipi_received still 0). QEMU's delivery path (gicv3_redist_send_sgi
     * setting gicr_ipendr0) orders only its own state, never our request fields. Under
     * TCG a guest `dsb ish` lowers to a host DMB on this CPU thread -- which is exactly
     * what the protocol needs here. */
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
    __asm__ volatile("msr " ICC_SGI1R_EL1 ", %0" :: "r"(sgi));

    unsigned tries = 0; /* TODO(diag SMP B7): drop with the hang debugging */
    while (__atomic_load_n(&g_invalidate_tlb_request.num_responses, __ATOMIC_ACQUIRE)
            != g_num_cpus) {
        /* Waiting for the other CPUs' EL1 IRQ dispatch (isr_c SGI case) to flush and
         * acknowledge. Every target unmasks IRQs in bounded time: EL1 C runs with DAIF.I
         * masked only inside the scheduler's own save/restore windows, and the SGI handler
         * itself never blocks nor schedules (it is the SGI path *checking* this counter,
         * so it must not run on this CPU -- and by definition it cannot: we are that CPU).
         * A CPU stuck in the page-table lock is no problem either: the lock holder here is
         * about to finish as soon as the last ack lands. */
        CPU_RELAX();
        if (++tries == 20000000) {
            tries = 0;
            log_always("shootdown: resp=%u/%u recv[%u %u %u %u]",
                       __atomic_load_n(&g_invalidate_tlb_request.num_responses,
                                       __ATOMIC_RELAXED),
                       g_num_cpus,
                       g_num_cpus > 0 ? g_per_cpu_data[0].invalidate_tlb_ipi_received : 0,
                       g_num_cpus > 1 ? g_per_cpu_data[1].invalidate_tlb_ipi_received : 0,
                       g_num_cpus > 2 ? g_per_cpu_data[2].invalidate_tlb_ipi_received : 0,
                       g_num_cpus > 3 ? g_per_cpu_data[3].invalidate_tlb_ipi_received : 0);
            /* TODO(diag SMP B7): liveness + redistributor ground truth per CPU */
            log_always("shootdown: ticks[%u %u %u %u]",
                       g_dbg_ticks[0], g_dbg_ticks[1], g_dbg_ticks[2], g_dbg_ticks[3]);
            log_always("shootdown: redist[0=%02x 1=%02x 2=%02x 3=%02x] (pend|en<<1|grp<<2|wake<<4)",
                       gic_redist_sgi1_state(0), gic_redist_sgi1_state(1),
                       gic_redist_sgi1_state(2), gic_redist_sgi1_state(3));
            /* raw INTID0..31 words: bit1 = shootdown SGI, bit30 = timer PPI */
            log_always("shootdown: pend[%08x %08x %08x %08x] en[%08x %08x %08x %08x]",
                       gic_redist_pend30(0), gic_redist_pend30(1),
                       gic_redist_pend30(2), gic_redist_pend30(3),
                       gic_redist_en30(0), gic_redist_en30(1),
                       gic_redist_en30(2), gic_redist_en30(3));
            /* IPRIORITYR word0: byte1 = shootdown SGI prio (0x20 = configured,
             * 0xff/0x00 = never written by lapic_timer_init on this CPU) */
            log_always("shootdown: prio[%08x %08x %08x %08x]",
                       gic_redist_prio0(0), gic_redist_prio0(1),
                       gic_redist_prio0(2), gic_redist_prio0(3));
            /* ACTIVE bits: QEMU's hppi skips pending-while-active, so act bit1 set on a
             * non-responder with pend bit1 = the exact wedge. */
            log_always("shootdown: act[%08x %08x %08x %08x]",
                       gic_redist_act30(0), gic_redist_act30(1),
                       gic_redist_act30(2), gic_redist_act30(3));
            /* Corrective nudge (diag): after a few settled dumps, force-clear ACTIVE and
             * re-pend the shootdown SGI on every non-responder (not this CPU) that has it
             * pending. If the round then completes, the loss was redistributor state. */
            {
                static uint32_t dbg_dump_no;
                static uint32_t dbg_nudged_mask;
                uint32_t self = get_per_cpu_data()->cpu_id;
                if (++dbg_dump_no >= 3) {
                    for (uint32_t c = 0; c < g_num_cpus && c < 8; c++) {
                        if (c == self || (dbg_nudged_mask & (1u << c)))
                            continue;
                        if (g_per_cpu_data[c].invalidate_tlb_ipi_received)
                            continue;
                        uint32_t pend = gic_redist_pend30(c);
                        if (pend & 0x2) {
                            dbg_nudged_mask |= 1u << c;
                            log_always("shootdown: nudging cpu%u (pend=%08x act=%08x)",
                                       c, pend, gic_redist_act30(c));
                            gic_redist_nudge_sgi1(c);
                        }
                    }
                }
            }
            /* last 8 IAR/EOI events per cpu (bit10=EOI, low bits=INTID), oldest first */
            for (uint32_t c = 0; c < g_num_cpus && c < 8; c++) {
                uint32_t base = g_dbg_event_idx[c];
                log_always("shootdown: cpu%u ev[%x %x %x %x %x %x %x %x]", c,
                           g_dbg_events[c][(base - 8) & 15], g_dbg_events[c][(base - 7) & 15],
                           g_dbg_events[c][(base - 6) & 15], g_dbg_events[c][(base - 5) & 15],
                           g_dbg_events[c][(base - 4) & 15], g_dbg_events[c][(base - 3) & 15],
                           g_dbg_events[c][(base - 2) & 15], g_dbg_events[c][(base - 1) & 15]);
                /* what a preemptive resume would eret to on this cpu: poisoned (masked) spsr
                 * there = the CPU runs its EL0 thread deaf */
                uintptr_t gs = replace_with_null_if_dummy_gs_base(
                    (uintptr_t)g_per_cpu_data[c].cur_tcb);
                struct thread* t = gs ? get_thread_ptr(gs) : NULL;
                log_always("shootdown: cpu%u cur spsr=%08lx elr=%08lx kpc=%08lx daif=%lx",
                           c, t ? (unsigned long)t->irq_pseudo_stack.spsr : 0xfffffff0UL,
                           t ? (unsigned long)t->irq_pseudo_stack.elr : 0,
                           t ? (unsigned long)t->context.pc : 0,
                           t ? (unsigned long)t->context.pstate : 0);
                /* TODO(diag SMP B7): asm liveness breadcrumb: [0]=last TRACE point of any
                 * kind, [1]=last exception entry. 0 => this vCPU never even reached an
                 * instrumented entry; hang/el1_sync addresses => it vanished there. */
                log_always("shootdown: cpu%u trace=%016lx irq=%016lx", c,
                           (unsigned long)g_per_cpu_data[c].dbg_irq_trace[0],
                           (unsigned long)g_per_cpu_data[c].dbg_irq_trace[1]);
            }
        }
    }

    g_invalidate_tlb_request.num_responses = 0;
    g_invalidate_tlb_request.in_progress = false;
    ret = 0;

out:
    spinlock_unlock(&g_invalidate_tlb_request_lock);
    return ret;
}

int interrupts_init(void) {
    /* The vector table itself is installed by syscalls_init() (VBAR_EL1) before we get here;
     * the timer and its interrupt line were configured by gic_init()/lapic_timer_init().
     * Unmask Group-1 IRQs at EL1 now that everything that must be ready is ready. */
    sti();
    g_interrupts_enabled = true;
    return 0;
}
