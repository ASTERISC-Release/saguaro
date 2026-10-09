/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/* Device Tree blob accessors (see kernel_dtb.c). */

#pragma once

#include <stdint.h>

/* Physical address and size of the first /memory bank (fallback constants until the real
 * /memory parser lands; QEMU virt boots with DRAM at 0x40000000). */
extern uint64_t g_dtb_mem_start;
extern uint64_t g_dtb_mem_size;

/* Initialize from the x0 boot pointer. Returns 0 on a well-formed DTB header, -1 otherwise. */
int dtb_init(const void* fdt);

/* Generic timer frequency in Hz (DTB /timer clock-frequency with CNTFRQ_EL0 fallback). */
uint64_t dtb_cntfrq(void);

/* TODO(Milestone-B): int dtb_find_compatible(const char* compat, uint64_t out_reg[], ...); */
