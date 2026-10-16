/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* AMD SEV / SEV-SNP support of the vm PAL (Documentation/devel/sev/PLAN.md). Included by assembly
 * too: the constants have no C suffixes. */

#pragma once

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#define MSR_AMD64_SEV_STATUS 0xc0010131
#define SEV_STATUS_SEV       (1 << 0)
#define SEV_STATUS_SEV_ES    (1 << 1)
#define SEV_STATUS_SEV_SNP   (1 << 2)

/* Fixed guest-physical pages of the SNP boot image (gramine-sev-igvm.py writes them), inside the
 * PAL's reserved range [1 MB, 4 MB) so that they stay mapped (kernel-only) for the whole run */
#define SEV_SNP_SECRETS_PAGE_GPA 0x3fe000
#define SEV_SNP_CPUID_PAGE_GPA   0x3ff000
/* value of ESI at the SNP entry (the IGVM VMSA sets it); plain SEV enters through the reset stub */
#define SEV_SNP_BOOT_MAGIC       0x21504e53 /* "SNP!" */

/* sev_terminate() reason codes (the hypervisor logs them) */
#define SEV_TERM_GENERAL         0x01
#define SEV_TERM_GHCB_PROTOCOL   0x02
#define SEV_TERM_GHCB_REGISTER   0x03
#define SEV_TERM_GHCB_CALL       0x04
#define SEV_TERM_PVALIDATE       0x05
#define SEV_TERM_PSC             0x06
#define SEV_TERM_VC_UNHANDLED    0x07
#define SEV_TERM_NOT_SNP         0x08
#define SEV_TERM_REVALIDATE      0x09

#define SEV_SNP_REPORT_SIZE      1184 /* ATTESTATION_REPORT of the SNP firmware ABI */

#define SEV_PVALIDATE_NOCHANGE   0x100
#define SEV_PVALIDATE_FAIL_SIZEMISMATCH 6

#ifndef __ASSEMBLER__

/* set by vm_bootloader.S before paging is enabled */
extern uint64_t g_sev_c_bit_mask;
extern uint64_t g_sev_status;

/* true under SEV-ES/SNP: intercepted instructions raise #VC, host access goes through the GHCB */
static inline bool sev_es_active(void) {
    return g_sev_status & (SEV_STATUS_SEV_ES | SEV_STATUS_SEV_SNP);
}

static inline bool sev_snp_active(void) {
    return g_sev_status & SEV_STATUS_SEV_SNP;
}

/* Checks that the guest runs with SEV active (refuses to boot otherwise), sets up the boot GHCB and
 * the early #VC handler under SEV-ES/SNP, and logs the state. The first call of pal_start_c(). */
void sev_init_early(void);

/* Physical page that stays shared besides the shared heap and the MMIO hole (the boot GHCB), or 0 */
extern uint64_t g_sev_extra_shared_page;

/* kernel_memory.c hooks (SNP; no-ops otherwise): validate the pages of the page tables before
 * they are written, low memory after the temporary tables; after the final ones set up the
 * validation bitmap and make the shared heap shared; memory_alloc() validates on demand */
void sev_memory_before_page_tables(uint64_t addr, size_t size);
void sev_memory_after_temp_tables(void);
void sev_memory_after_final_tables(uint64_t memory_end, uint64_t tables_end);
int sev_validate_on_demand(uint64_t addr, size_t size);
uint64_t sev_validated_pages(void);
extern uint8_t* g_sev_valid_bitmap;
extern uint64_t g_sev_valid_bitmap_pages;
extern uint64_t g_sev_vc_late_validations; /* #VC-driven validations (diagnostics) */

/* Per-CPU SEV state for `num_cpus` vCPUs (#VC stacks, GHCBs of the APs); after
 * init_multicore_prepare(), before interrupts_init(). */
int sev_percpu_prepare(uint32_t num_cpus);
/* top of the #VC IST stack of `cpu_id` (TSS IST2) */
uint64_t sev_vc_stack_top(uint32_t cpu_id);

/* SNP AP start (the INIT-SIPI sequence does not exist for SEV-ES/SNP guests): creates vCPU
 * `cpu_idx` (APIC ID = cpu_idx) with a VMSA that enters `entry(cpu_idx)` in 64-bit mode on the
 * current page tables, GDT and CR4 */
int sev_snp_start_ap(uint32_t cpu_idx, void (*entry)(uint32_t));

/* first C code of an AP under SNP (called by the AP's VMSA entry): GHCB, early #VC handler */
void sev_ap_early(uint32_t cpu_idx);

/* An SNP attestation report with `user_data` as its REPORT_DATA (sev_attest.c) */
int sev_snp_get_report(const uint8_t user_data[64], uint8_t report[SEV_SNP_REPORT_SIZE]);

/* sev_vc.c, sev_ghcb.c (general-regs-only) */
uint32_t sev_this_cpu_id(void);
void sev_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);
int sev_pvalidate(uint64_t va, bool huge, bool validate);
int sev_psc_msr(uint64_t gpa, bool shared);
int sev_psc_range(uint64_t gpa, uint64_t size, bool shared);
void sev_boot_pte_set_shared(uint64_t gpa);
void sev_vc_early_idt_init(void);
void isr_vc(void);

#endif /* __ASSEMBLER__ */
