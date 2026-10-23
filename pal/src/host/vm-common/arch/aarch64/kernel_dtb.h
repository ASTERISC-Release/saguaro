/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Device Tree blob accessors (see kernel_dtb.c). */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Physical address and size of the first /memory bank, parsed by dtb_init() (a boot without a
 * parseable /memory is rejected; QEMU virt always describes DRAM@0x40000000). */
extern uint64_t g_dtb_mem_start;
extern uint64_t g_dtb_mem_size;

/* Initialize from the x0 boot pointer: validates the header AND parses /memory (required),
 * /cpus and /psci. Returns 0 on a usable DTB, -1 on any malformed or memory-less blob. */
int dtb_init(const void* fdt);

/* Number of device_type=="cpu" nodes under /cpus (floored to 1; capped at MAX_NUM_CPUS). */
uint32_t dtb_num_cpus(void);

/* PSCI CPU_ON target id of the idx-th /cpus/cpu@N `reg` (PSCI's cpu id is the DT reg value,
 * *not* the MPIDR); 0 (cpu 0) for idx 0, 0 past the parsed range. */
uint32_t dtb_cpu_on_id(uint32_t idx);

/* Whether the blob describes the PSCI conduit ("arm,psci*" compatible) and asks for HVC
 * instead of the default SMC. */
bool dtb_psci_available(void);
bool dtb_psci_conduit_hvc(void);

/* Generic timer frequency in Hz (DTB /timer clock-frequency with CNTFRQ_EL0 fallback). */
uint64_t dtb_cntfrq(void);

/* TODO(Milestone-B): int dtb_find_compatible(const char* compat, uint64_t out_reg[], ...); */
