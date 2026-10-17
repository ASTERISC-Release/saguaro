/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The VMSA (SEV-ES save area, AMD APM vol. 2, table B-4) of SNP AP creation; offsets as in the
 * igvm crate's SevVmsa and QEMU's struct sev_es_save_area. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define SEV_FEATURE_SNP_ACTIVE (1UL << 0)

struct sev_vmsa_seg {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base;
} __attribute__((packed));

struct sev_vmsa {
    struct sev_vmsa_seg es, cs, ss, ds, fs, gs;  /* 0x000 */
    struct sev_vmsa_seg gdtr, ldtr, idtr, tr;    /* 0x060 */
    uint64_t pl0_ssp, pl1_ssp, pl2_ssp, pl3_ssp, u_cet; /* 0x0a0 */
    uint8_t  reserved_0xc8[2];
    uint8_t  vmpl;                               /* 0x0ca */
    uint8_t  cpl;                                /* 0x0cb */
    uint8_t  reserved_0xcc[4];
    uint64_t efer;                               /* 0x0d0 */
    uint8_t  reserved_0xd8[104];
    uint64_t xss;                                /* 0x140 */
    uint64_t cr4;                                /* 0x148 */
    uint64_t cr3;                                /* 0x150 */
    uint64_t cr0;                                /* 0x158 */
    uint64_t dr7;                                /* 0x160 */
    uint64_t dr6;                                /* 0x168 */
    uint64_t rflags;                             /* 0x170 */
    uint64_t rip;                                /* 0x178 */
    uint64_t dr0, dr1, dr2, dr3;                 /* 0x180 */
    uint64_t dr0_addr_mask, dr1_addr_mask, dr2_addr_mask, dr3_addr_mask; /* 0x1a0 */
    uint8_t  reserved_0x1c0[24];
    uint64_t rsp;                                /* 0x1d8 */
    uint64_t s_cet, ssp, isst_addr;              /* 0x1e0 */
    uint64_t rax;                                /* 0x1f8 */
    uint64_t star, lstar, cstar, sfmask;         /* 0x200 */
    uint64_t kernel_gs_base;                     /* 0x220 */
    uint64_t sysenter_cs, sysenter_esp, sysenter_eip; /* 0x228 */
    uint64_t cr2;                                /* 0x240 */
    uint8_t  reserved_0x248[32];
    uint64_t g_pat;                              /* 0x268 */
    uint64_t dbgctl;                             /* 0x270 */
    uint64_t br_from, br_to, last_excp_from, last_excp_to; /* 0x278 */
    uint8_t  reserved_0x298[72];
    uint64_t spec_ctrl;                          /* 0x2e0 */
    uint8_t  reserved_0x2e8[32];
    uint64_t rcx, rdx, rbx;                      /* 0x308 */
    uint64_t reserved_0x320;
    uint64_t rbp, rsi, rdi;                      /* 0x328 */
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15; /* 0x340 */
    uint8_t  reserved_0x380[16];
    uint64_t guest_exit_info_1, guest_exit_info_2, guest_exit_int_info, guest_nrip; /* 0x390 */
    uint64_t sev_features;                       /* 0x3b0 */
    uint64_t vintr_ctrl;                         /* 0x3b8 */
    uint64_t guest_exit_code;                    /* 0x3c0 */
    uint64_t virtual_tom;                        /* 0x3c8 */
    uint64_t tlb_id, pcpu_id;                    /* 0x3d0 */
    uint64_t event_inj;                          /* 0x3e0 */
    uint64_t xcr0;                               /* 0x3e8 */
    uint8_t  reserved_0x3f0[16];
    uint64_t x87_dp;                             /* 0x400 */
    uint32_t mxcsr;                              /* 0x408 */
    uint16_t x87_ftw, x87_fsw, x87_fcw, x87_fop; /* 0x40c */
    uint16_t x87_ds, x87_cs;                     /* 0x414 */
    uint64_t x87_rip;                            /* 0x418 */
} __attribute__((packed));

_Static_assert(offsetof(struct sev_vmsa, efer) == 0xd0, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, rip) == 0x178, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, rax) == 0x1f8, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, g_pat) == 0x268, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, rcx) == 0x308, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, sev_features) == 0x3b0, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, xcr0) == 0x3e8, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, mxcsr) == 0x408, "VMSA layout");
_Static_assert(offsetof(struct sev_vmsa, x87_fcw) == 0x410, "VMSA layout");
