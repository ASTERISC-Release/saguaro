/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_apic.c: GICv2/v3 distributor initialization and the
 * ARM Generic Timer as the kernel tick source (x86: local APIC timer in TSC-deadline mode).
 *
 * Notes on multi-core synchronization: only boot-time init happens here; the IRQ fast path
 * (IAR/EOI) is used from interrupt context via lapic_* and is single-register, lock-free.
 */

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "pal_error.h"

#include "kernel_apic.h"
#include "kernel_multicore.h"

static bool g_gicv3 = false; /* decided in gic_init() */

static inline uint32_t gicd_read(uint32_t off) {
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"((const volatile void*)(GICD_BASE + off))
                     : "memory");
    return v;
}

static inline void gicd_write(uint32_t off, uint32_t val) {
    __asm__ volatile("stlr %w0, [%1]" :: "r"(val),
                     "r"((volatile void*)(GICD_BASE + off)) : "memory");
}

/*
 * Redistributor frames are per-CPU: QEMU virt's DTB `intc@8000000` reg describes one
 * contiguous region (0x80a0000, 0xf60000 = 128K per possible CPU) whose frames follow the
 * mpidr order of the cpu@N nodes. With QEMU's linear mpidr (Aff0 == cpu index for the first
 * 256 CPUs -- which is all MAX_NUM_CPUS allows anyway) the frame for a CPU is therefore
 * GICR_BASE + cpu_id * GICR_SIZE. Each CPU -- APs included -- must wake and configure *its
 * own* frame before its private timer PPI can be delivered to its CPU interface.
 */
static inline uintptr_t gic_redist_base_this_cpu(void) {
    return GICR_BASE + (uintptr_t)get_per_cpu_data()->cpu_id * GICR_SIZE;
}

static inline uint32_t gicr_read(uintptr_t base, uint32_t off) {
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"((const volatile void*)(base + off))
                     : "memory");
    return v;
}

static inline void gicr_write(uintptr_t base, uint32_t off, uint32_t val) {
    __asm__ volatile("stlr %w0, [%1]" :: "r"(val),
                     "r"((volatile void*)(base + off)) : "memory");
}

static inline void gic_set_priority(uint32_t intid, uint8_t prio) {
    /* IPRIORITYR packs one byte per INTID; same layout on GICv2 and v3 */
    uint32_t off = GICD_IPRIORITYR(intid / 4);
    uint32_t word = gicd_read(off);
    word = (word & ~(0xffu << (8 * (intid % 4)))) | ((uint32_t)prio << (8 * (intid % 4)));
    gicd_write(off, word);
}

static void gic_enable_intid(uint32_t intid) {
    gicd_write(GICD_ISENABLER(intid / 32), 1u << (intid % 32));
}

static bool gic_is_v3(void) {
    /* The GIC architecture spec's *only* version query: GICD_PIDR2 (Peripheral ID register,
     * offset 0xff8 in the first 64K sector), nibble ARCH_FIELD: 0b0010 = v2, 0b0011 = v3.
     * Probing GICD_TYPER's ITS bit is wrong: QEMU's GICv3 sets it only when a GITS node was
     * created (never on plain `-machine virt,gic-version=3`), so it misreads as v2 and the
     * distributor-based PPI enable then silently no-ops (PPIs live on the redistributor).
     * ICC_SRE_EL1 would be even more authoritative but traps on cores/QEMU without SRE. */
    return ((gicd_read(GICD_PIDR2) >> 4) & 0xf) == 0x3;
}

void lapic_signal_interrupt_complete(uint32_t intid) {
    /* GICv3 ICC_EOIR1_EL1 is a write-with-INTID register (the value must be the INTID that the
     * last ICC_IAR1_EL1 read returned): QEMU's icc_eoir_write() performs the priority drop for
     * the highest active group and then icc_deactivate_irq(cs, <written value>). EOI'ing with a
     * wrong INTID (e.g. 0, the x86-style "end current interrupt" write) drops the priority but
     * leaves the interrupt ACTIVE forever -- gicr_int_pending() then masks every subsequent
     * pending via `pend &= ~gicr_iactiver0` and only the first tick is ever delivered. */
    __asm__ volatile("msr " ICC_EOI1_EL1 ", %0" :: "r"((uint64_t)intid));
}

uint32_t gic_acknowledge_intid(void) {
    uint64_t intid;
    /* ICC_IAR1_EL1: acknowledge + priority drop; returns 1023 for spurious */
    __asm__ volatile("mrs %0, " ICC_IAR1_EL1 : "=r"(intid));
    return (uint32_t)intid;
}

void lapic_enable(void) {
    /* unmask Group-1 IRQs with priority <= 0xf0 (our priorities are 0x10/0x20) */
    uint64_t pmr = 0xf0;
    __asm__ volatile("msr " ICC_PMR_EL1 ", %0" :: "r"(pmr));
    /* The CPU interface gates Group-1 delivery on ICC_IGRPEN1_EL1.Enable *in addition* to
     * the priority mask (x86: the LAPIC has a separate spurious-interrupt enable bit; PMR
     * alone is not enough). Its reset value of 0 silently drops every pending IRQ -- the
     * redistributor latches PPI/SPI pendings fine, but icc_no_enabled_hppi() then reports
     * "no enabled hppi" forever. */
    __asm__ volatile("msr " ICC_IGRPEN1_EL1 ", %0" :: "r"((uint64_t)1));
}

int gic_spi_enable(uint32_t intid) {
    if (intid < 32 || intid >= GICD_SPI_MAX)
        return -PAL_ERROR_INVAL;

    if (g_gicv3) {
        /* non-secure Group 1: IGROUPR bit = 1, IGRPMODR bit = 0 (reset default) */
        uint32_t word = gicd_read(GICD_IGROUPR(intid / 32));
        gicd_write(GICD_IGROUPR(intid / 32), word | (1u << (intid % 32)));
        /* route to affinity 0.0.0.0 (CPU0) via the memory-mapped IROUTER (two 32-bit halves:
         * little-endian, low word first); all device handling happens on CPU0 */
        gicd_write(GICD_IROUTER(intid), 0 /* low 32 bits of affinity */);
        gicd_write(GICD_IROUTER(intid) + 4, 0 /* high 32 bits */);
    } else {
        /* GICv2: target list byte, bit0 = CPU0 */
        uint32_t off = GICD_ITARGETSR(intid / 4);
        uint32_t word = gicd_read(off);
        word = (word & ~(0xffu << (8 * (intid % 4)))) | (1u << (8 * (intid % 4)));
        gicd_write(off, word);
    }
    gic_set_priority(intid, 0x20);
    gic_enable_intid(intid);
    return 0;
}

int lapic_timer_init(void) {
    /* The EL1 physical timer is PPI 30; on GICv3 its redistributor frame needs WAKER wakeup
     * and the SGI_base mirror of the distributor registers before the line can be enabled.
     * Called once per CPU (BSP from gic_init(), each AP from pal_start_ap_c()): every access
     * below is either this CPU's redistributor frame or a per-CPU sysreg. */
    if (g_gicv3) {
        uintptr_t rdbase = gic_redist_base_this_cpu();
        gicr_write(rdbase, GICR_CTLR, 0);
        gicr_write(rdbase, GICR_WAKER, 0); /* clear Sleep bit; wake this CPU interface */
        while (gicr_read(rdbase, GICR_WAKER) & (1u << 2)) /* ChildrenAsleep */
            ;
        /* PPI 30 on the redistributor SGI_base mirror: group non-secure, priority, enable */
        gicr_write(rdbase, GICR_SGI_BASE + GICD_IGROUPR(30 / 32), 1u << (30 % 32));
        gicr_write(rdbase, GICR_SGI_BASE + GICD_IPRIORITYR(30 / 4), 0x10u << (8 * (30 % 4)));
        gicr_write(rdbase, GICR_SGI_BASE + GICD_ISENABLER(30 / 32), 1u << (30 % 32));
    } else {
        gic_set_priority(GIC_INTID_PPI_EL1_PHYS, 0x10);
        gic_enable_intid(GIC_INTID_PPI_EL1_PHYS);
    }

    /* one-shot: program CNTP_TVAL_EL0 and enable CNTP_CTL_EL0 (timer fires at TVAL==0) */
    uint64_t cntfrq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    if (!cntfrq)
        return -PAL_ERROR_INVAL;
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)0)); /* disable while reprogramming */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(cntfrq / 10)); /* 100ms period */
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1));  /* ENABLE */
    return 0;
}

void lapic_timer_rearm(void) {
    uint64_t cntfrq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(cntfrq / 10));
}

int gic_init(void) {
    /* the distributor must be reachable: on a bus error QEMU returns all-ones */
    uint32_t typer = gicd_read(GICD_TYPER);
    if (typer == 0xffffffff)
        return -PAL_ERROR_INVAL;

    g_gicv3 = gic_is_v3();

    if (g_gicv3) {
        /* GICv3 GICD_CTLR: EnableGrp1S (bit0) + EnableGrp1NS (bit1); disable bypass of affinity
         * routing (bits 3:2 reset to 0b01 = route by IROUTER, what we program). */
        gicd_write(GICD_CTLR, (1u << 0) | (1u << 1));
    } else {
        /* GICv2: enable the distributor (secure-only model in QEMU without TZ: bit0) */
        gicd_write(GICD_CTLR, 1);
    }

    lapic_enable();

    /* pre-arm the virtio-mmio SPI range (devices exist at fixed addresses on QEMU virt; the
     * drivers pick their INTIDs via gic_spi_enable() once BARs are enumerated) */
    return lapic_timer_init();
}
