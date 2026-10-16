/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* AMD SEV / SEV-SNP support of the vm PAL (Documentation/devel/sev/PLAN.md). */

#include "api.h"
#include "pal_internal.h"

#include "kernel_memory.h"
#include "sev.h"

void sev_init_early(void) {
    if (!(g_sev_status & SEV_STATUS_SEV) || !g_sev_c_bit_mask) {
        /* the SEV PAL keeps nothing secret without encryption: refuse to run */
        INIT_FAIL("SEV: memory encryption is not active (SEV_STATUS 0x%lx); this PAL runs only "
                  "as an AMD SEV guest (use gramine-vm otherwise)", g_sev_status);
    }

    log_always("SEV: memory encryption active: %s%s%s (SEV_STATUS 0x%lx), C-bit %d",
               "SEV", (g_sev_status & SEV_STATUS_SEV_ES) ? " SEV-ES" : "",
               (g_sev_status & SEV_STATUS_SEV_SNP) ? " SEV-SNP" : "", g_sev_status,
               __builtin_ctzl(g_sev_c_bit_mask));
    log_always("SEV: shared (unencrypted) guest memory: [0x%lx, 0x%lx) for virtio, [0x%lx, 0x%lx) "
               "MMIO; everything else private", SHARED_MEM_ADDR, SHARED_MEM_ADDR + SHARED_MEM_SIZE,
               PCI_HOLE_ADDR, PCI_HOLE_ADDR + PCI_HOLE_SIZE);
}
