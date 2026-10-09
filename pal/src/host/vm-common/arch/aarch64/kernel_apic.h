/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_apic.h.
 *
 * The GIC (Generic Interrupt Controller) replaces the local APIC; the x86 "lapic_*" names are
 * kept for the functions shared device drivers call (isr dispatch, timer rearm), so the drivers
 * (virtio console, fs, vsock) stay architecture-neutral. QEMU virt GICv2/v3 memory map (from
 * qemu hw/arm/virt.c): distributor at 0x08000000, redistributors at 0x080a0000 (one 128K frame
 * per CPU), which is also why the bootloader's initial page tables map [0x08000000, 0x0a000000)
 * as device-nGnRE memory.
 *
 * Interrupts are configured as non-secure Group-1 so they are delivered as ordinary EL1 IRQs and
 * consumed through ICC_IAR1_EL1/ICC_EOI1_EL1.
 */

#pragma once

#include <stdint.h>

#define GICD_BASE   0x08000000UL
#define GICR_BASE   0x080a0000UL
#define GICR_SIZE   0x20000UL       /* per-CPU redistributor frame */

/* Interrupt numbers (INTIDs) as routed on QEMU virt */
#define GIC_INTID_PPI_EL1_PHYS      30 /* per-CPU private physical timer (PPI) */
#define GIC_INTID_SPI_VIRTIO1       32 /* virtio-mmio devices start here (offset by index) */
#define GIC_INTID_SPI_UART0         33 /* PL011 (unused by the kernel: PL011 is polled) */

/* GIC supports INTIDs 0..1023 (SPIs are 32..1023 on QEMU virt without LPIs) */
#define GICD_SPI_MAX 1024

/* GICD register byte offsets (common v2/v3 numbering; v2/v3 divergence noted where relevant) */
#define GICD_CTLR                   0x000
#define GICD_TYPER                  0x004
#define GICD_IIDR                   0x008
/* GICD_PIDR2 (Peripheral ID 2): the ID register block of the 64K GICD frame QEMU maps sits
 * at 0xffd0..0xffff (not the repacked 4K-page layout), PIDR2 = third word of the PID half
 * at +0x18 (0xffe8); bits [7:4] = architecture revision (0b0010 = v2, 0b0011 = v3).
 * Read-only. */
#define GICD_PIDR2                  0xffe8
#define GICD_IGROUPR(n)             (0x080 + 4 * (n)) /* v3; v2: secure-only, RAZ/WI in NS */
#define GICD_ISENABLER(n)           (0x100 + 4 * (n))
#define GICD_ICENABLER(n)           (0x180 + 4 * (n))
#define GICD_ICPENDR(n)             (0x280 + 4 * (n))
#define GICD_IPRIORITYR(n)          (0x400 + 4 * (n)) /* byte per INTID, packed 4/word */
#define GICD_ITARGETSR(n)           (0x800 + 4 * (n)) /* v2 only: byte per INTID */
#define GICD_ICFGR(n)               (0xc00 + 4 * (n))
#define GICD_IGRPMODR(n)            (0xd00 + 4 * (n)) /* v3 only */
#define GICD_IROUTER(n)             (0x6000 + 8 * (n)) /* v3 only: 64-bit affinity route */

/* GICv3 redistributor: SGI/PPI view mirrors the distributor registers at SGI_BASE */
#define GICR_CTLR                   0x000
#define GICR_WAKER                  0x0040
#define GICR_SGI_BASE               0x10000

/* ICC_* system registers (GIC CPU interface, mrs/msr at EL1) */
#define ICC_PMR_EL1                 "s3_0_c4_c6_0"
#define ICC_IGRPEN1_EL1             "s3_0_c12_c12_7"
#define ICC_IAR1_EL1                "s3_0_c12_c12_0"
#define ICC_EOI1_EL1                "s3_0_c12_c12_1"

void lapic_enable(void);                   /* unmask Group-1 IRQs via ICC_PMR_EL1 */
int  lapic_timer_init(void);               /* EL1 physical generic timer + its PPI line */
void lapic_timer_rearm(void);              /* reload CNTP_TVAL_EL0 (one-shot semantics) */
void lapic_signal_interrupt_complete(uint32_t intid); /* ICC_EOIR1_EL1, write carries INTID */

/* Acknowledge the highest-priority pending Group-1 IRQ (ICC_IAR1_EL1); returns INTID
 * (1023 == spurious). Must be paired with lapic_signal_interrupt_complete(intid): GICv3's
 * ICC_EOIR1_EL1 is write-with-INTID and deactivates exactly the INTID it is given. */
uint32_t gic_acknowledge_intid(void);

/* Enable/route an SPI for a discovered device (virtio-mmio index -> SPI 32+index, CPU0). */
int gic_spi_enable(uint32_t intid);

/* Initialize the distributor: the aarch64 analog of x86 `apic_init()`. */
int gic_init(void);
