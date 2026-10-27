/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Local and I/O APIC, also called LAPIC and IOAPIC. Currently used only for IRQ routing (remapping
 * of IRQs of virtio devices provided by the hypervisor to interrupt vector number of CPU#0).
 *
 * Notes on multi-core synchronization:
 *   - I/O APIC operations (redirection of IRQs) happen only on init, no sync required
 *   - LAPIC operations are local to the CPU, no sync required
 *
 * References:
 *   - Local APIC: Intel SDM, Volume 3, Chapter 10
 *   - I/O APIC: https://pdos.csail.mit.edu/6.828/2016/readings/ia32/ioapic.pdf
 */

#include <stdint.h>

#include "api.h"
#include "cpu.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_apic.h"
#include "kernel_memory.h"
#include "kernel_time.h"
#include "vm_callbacks.h"

extern uint64_t g_tsc_mhz;

static void ioapic_write_reg(uint32_t offset, uint32_t val) {
    uint32_t* ioapic_regsel_addr = (uint32_t*)(IOAPIC_ADDR + 0x00);
    uint32_t* ioapic_win_addr    = (uint32_t*)(IOAPIC_ADDR + 0x10);

    vm_mmio_writel(ioapic_regsel_addr, offset);
    vm_mmio_writel(ioapic_win_addr, val);
}

__attribute__((unused))
static uint32_t ioapic_read_reg(uint32_t offset) {
    uint32_t* ioapic_regsel_addr = (uint32_t*)(IOAPIC_ADDR + 0x00);
    uint32_t* ioapic_win_addr    = (uint32_t*)(IOAPIC_ADDR + 0x10);

    vm_mmio_writel(ioapic_regsel_addr, offset);
    uint32_t val = vm_mmio_readl(ioapic_win_addr);

    /* NOTE: val was read from potentially malicious VMM, needs to be hardened when/if this function
     *       will become used */
    return val;
}

static void ioapic_redirect_irq(uint8_t irq, uint8_t interrupt_vector) {
    /* IOREDTBL (I/O Redirection Table) regs start at offset 0x10; each reg is comprised of 2 32-bit
     * ints -- we do not use the high 32 bits (which specify APIC ID), they are all-zeros */
    uint32_t offset = irq * 2 + 0x10;

    /* Redirection Table entry format: bits 63:56 -- APIC ID, 55:17 -- reserved, 16 -- interrupt
     * mask (r/w), 15 -- trigger mode (r/w), 14 -- remote IRR status (r/o), 13 - polarity (r/w), 12
     * -- delivery status (r/o), 11 -- destination mode (r/w), 10:8 -- delivery mode (r/w), 7:0 --
     * assigned interrupt vector (r/w).
     *
     * Out of these fields, we care about:
     *   - bits 63:56 -- APIC ID, currently set to zero (means CPU#0), see also bit 11
     *   - bit 15     -- trigger mode, set to "edge sensitive" (zero), virtio implies edge
     *   - bit 11     -- destination mode, currently set to zero (means "physical mode for APIC ID")
     *   - bits 10:8  -- delivery mode, currently `000` (means "Fixed", delivers interrupt to CPU)
     *   - bits 7:0   -- interrupt vector on which the device IRQs are delivered by the CPU
     */
    uint32_t val = interrupt_vector;
    ioapic_write_reg(offset, val);
}

void lapic_enable(void) {
    /* set up spurious interrupt register: with IRQ 39, APIC enabled */
    vm_shared_wrmsr(MSR_INSECURE_IA32_LAPIC_SPURIOUS_INTERRUPT_VECTOR, 39 | 0x100);
}

/* x2APIC ID of CPU0 (the BSP), the destination of lapic_timer_kick_cpu0(); read in apic_init() */
static uint32_t g_cpu0_apic_id = 0;

/* note that LAPIC timer is out of scope of e.g. Intel TDX, as TDX doesn't virtualize timer MSRs; in
 * other words, we must consider timer operations as insecure */
/* Without a TSC-deadline timer the PAL falls back to the one-shot mode of the LAPIC timer. This
 * happens for SEV-ES/SNP guests under KVM (KVM does not offer the TSC-deadline timer to guests with
 * protected state) and under QEMU TCG, which does not implement it. The count is decremented at the
 * APIC bus frequency of 1 GHz (1 ns per count, divide-by-1) */
#define MSR_INSECURE_IA32_LAPIC_TIMER_ICR 0x838
#define MSR_INSECURE_IA32_LAPIC_TIMER_DCR 0x83e
static bool g_lapic_timer_oneshot;

static void lapic_timer_oneshot_arm(uint64_t delay_us) {
    uint64_t count = delay_us * 1000;
    vm_shared_wrmsr(MSR_INSECURE_IA32_LAPIC_TIMER_ICR, count ? MIN(count, UINT32_MAX) : 1);
}

/* arms this CPU's timer `delay_us` from now; both modes are one-shot, a write replaces the pending
 * deadline (TSC-deadline mode, or the one-shot count mode when the CPU has no TSC-deadline timer) */
static void lapic_timer_arm(uint64_t delay_us) {
    if (g_lapic_timer_oneshot) {
        lapic_timer_oneshot_arm(delay_us);
        return;
    }
    uint64_t future_tsc = get_tsc() + delay_us * g_tsc_mhz;
    vm_shared_wrmsr(MSR_INSECURE_IA32_TSC_DEADLINE, future_tsc);
}

int lapic_timer_init(void) {
    assert(g_tsc_mhz);

    uint32_t words[CPUID_WORD_NUM];
    cpuid(FEATURE_FLAGS_LEAF, 0, words);
    if (!(words[CPUID_WORD_ECX] & (1 << 24))) {
        g_lapic_timer_oneshot = true;
        vm_shared_wrmsr(MSR_INSECURE_IA32_LAPIC_TIMER_DCR, 0xb); /* divide by 1 */
        vm_shared_wrmsr(MSR_INSECURE_IA32_LAPIC_LVT_TIMER, 32);  /* one-shot, IRQ 32 */
        lapic_timer_arm(LAPIC_TIMER_PERIOD_US);
        return 0;
    }

    /* set up LVT timer: with IRQ 32, in TSC-Deadline mode (bit 18 set), not masked */
    vm_shared_wrmsr(MSR_INSECURE_IA32_LAPIC_LVT_TIMER, 32 | 0x40000);

    /* arm the timer for the first time */
    lapic_timer_arm(LAPIC_TIMER_PERIOD_US);
    return 0;
}

void lapic_timer_rearm(uint64_t delay_us) {
    lapic_timer_arm(delay_us);
    lapic_signal_interrupt_complete();
}

void lapic_timer_set_deadline(uint64_t delay_us) {
    lapic_timer_arm(delay_us);
}

void lapic_timer_kick_cpu0(void) {
    /* ICR: vector 32 (the timer vector, so CPU0's ISR runs unchanged), fixed delivery (bits 8-10
     * zero), physical destination, level asserted (bit 14), edge triggered, no shorthand,
     * destination x2APIC ID in the high dword; in x2APIC mode the write does not block */
    vm_shared_wrmsr(MSR_INSECURE_IA32_LAPIC_ICR, ((uint64_t)g_cpu0_apic_id << 32) | (1 << 14) | 32);
}

/* Makes the timer interrupt fire as soon as interrupts are enabled (a deadline in the past). */
void lapic_timer_fire_now(void) {
    if (g_lapic_timer_oneshot) {
        lapic_timer_oneshot_arm(0);
        return;
    }
    vm_shared_wrmsr(MSR_INSECURE_IA32_TSC_DEADLINE, get_tsc());
}

void lapic_signal_interrupt_complete(void) {
    /* write End-Of-Interrupt reg of LAPIC (according to x2APIC spec, 0 is the only valid value) */
    wrmsr(MSR_IA32_LAPIC_EOI, 0);
}

int apic_init(void) {
    /* IOAPIC initialization */
    int ret = memory_mark_pages_strong_uncacheable(IOAPIC_ADDR, IOAPIC_SIZE, /*mark=*/true);
    if (ret < 0)
        return ret;

    /* all device IRQs are multiplexed on the same CPU interrupt vector 64; we assume that device
     * IRQs span 0 to 31 */
    for (uint8_t irq = 0; irq < 32; irq++)
        ioapic_redirect_irq(irq, /*interrupt_vector=*/64);

    /* LAPIC initialization (in x2APIC mode); note that LAPIC should be already enabled and in
     * x2APIC mode, so no need to check or modify the IA32_APIC_BASE MSR */
    lapic_enable();

    /* apic_init() runs on the BSP (pal_main), so this is CPU0's ID; read through the PAL's callback
     * (in a TD a plain RDMSR is a #VE, and this runs before interrupts_init()) */
    uint64_t apic_id;
    ret = vm_shared_rdmsr(MSR_IA32_LAPIC_ID, &apic_id);
    if (ret < 0) {
        log_warning("Cannot read the x2APIC ID of CPU0 (%s); assuming 0", pal_strerror(ret));
        apic_id = 0;
    }
    g_cpu0_apic_id = (uint32_t)apic_id;

    return lapic_timer_init();
}
