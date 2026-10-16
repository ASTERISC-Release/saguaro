/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* AMD SEV / SEV-SNP support of the vm PAL (Documentation/devel/sev/PLAN.md). */

#pragma once

#include <stdint.h>

#define MSR_AMD64_SEV_STATUS 0xc0010131U
#define SEV_STATUS_SEV       (1UL << 0)
#define SEV_STATUS_SEV_ES    (1UL << 1)
#define SEV_STATUS_SEV_SNP   (1UL << 2)

/* set by vm_bootloader.S before paging is enabled */
extern uint64_t g_sev_c_bit_mask;
extern uint64_t g_sev_status;

/* Checks that the guest runs with SEV active (refuses to boot otherwise) and logs the state. */
void sev_init_early(void);
