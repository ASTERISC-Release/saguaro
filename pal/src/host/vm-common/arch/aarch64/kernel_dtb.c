/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Device Tree blob accessors for the aarch64 unikernel (replacement of the x86 ACPI/e820
 * information channel: QEMU virt passes a DTB at the start of RAM in x0 when booting via
 * -kernel, and it is the authoritative description of memory size, GIC/UART/PCIE addresses
 * and PSCI availability).
 *
 * Milestone-B status: STUB. Only validates the header; wired into pal_main.c so the boot
 * sequence has the right shape. Real node parsing (/memory, /interrupt-parent, /psci,
 * /pl011, /pci-host-ecam-generic, clock-frequency) comes with the bring-up commit.
 */

#include <stdint.h>

#include "api.h"

#include "kernel_dtb.h"

#define FDT_MAGIC 0xd00dfeedu

static const void* g_dtb = NULL;

uint64_t g_dtb_mem_start = 0;
uint64_t g_dtb_mem_size  = 0;

int dtb_init(const void* fdt) {
    uint32_t magic;
    if (!fdt)
        return -1;
    magic = __builtin_bswap32(*(const uint32_t*)fdt);
    if (magic != FDT_MAGIC)
        return -1;
    g_dtb = fdt;
    /* TODO(Milestone-B): parse /memory node into g_dtb_mem_start / g_dtb_mem_size */
    g_dtb_mem_start = 0x40000000UL;
    g_dtb_mem_size  = 0x80000000UL; /* default until real parse lands */
    return 0;
}

uint64_t dtb_cntfrq(void) {
    uint64_t freq;
    /* The generic counter frequency is directly readable at EL1 (CNTFRQ_EL0, initialized by
     * QEMU's firmware before entry), so we do not need the DTB /timer node for the common
     * case; keep the DTB path as a TODO fallback. */
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}
