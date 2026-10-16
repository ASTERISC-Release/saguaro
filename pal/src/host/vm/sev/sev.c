/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* AMD SEV / SEV-SNP support of the vm PAL (Documentation/devel/sev/PLAN.md). */

#include "api.h"
#include "pal_internal.h"

#include "kernel_interrupts.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_sched.h"
#include "sev.h"
#include "sev_ghcb.h"
#include "sev_vmsa.h"

#define SEV_VC_STACK_SIZE (4 * PAGE_SIZE)

static void* g_sev_vc_stacks[MAX_NUM_CPUS];

void sev_init_early(void) {
    if (!(g_sev_status & SEV_STATUS_SEV) || !g_sev_c_bit_mask) {
        /* the SEV PAL keeps nothing secret without encryption: refuse to run */
        INIT_FAIL("SEV: memory encryption is not active (SEV_STATUS 0x%lx); this PAL runs only "
                  "as an AMD SEV guest (use gramine-vm otherwise)", g_sev_status);
    }

    if (sev_es_active()) {
        /* everything that exits to the host from now on is a GHCB call or a #VC */
        sev_vc_early_idt_init();
        sev_ghcb_init_boot();
    }

    log_always("SEV: memory encryption active: %s%s%s (SEV_STATUS 0x%lx), C-bit %d",
               "SEV", sev_es_active() ? " SEV-ES" : "", sev_snp_active() ? " SEV-SNP" : "",
               g_sev_status, __builtin_ctzl(g_sev_c_bit_mask));
    log_always("SEV: shared (unencrypted) guest memory: [0x%lx, 0x%lx) for virtio, [0x%lx, 0x%lx) "
               "MMIO%s; everything else private", SHARED_MEM_ADDR,
               SHARED_MEM_ADDR + SHARED_MEM_SIZE, PCI_HOLE_ADDR, PCI_HOLE_ADDR + PCI_HOLE_SIZE,
               g_sev_extra_shared_page ? ", the boot GHCB page" : "");
}

/* ---- SNP memory validation ---- */

static uint64_t g_sev_validated_pages;

/* PVALIDATE [start, end) (identity-mapped, page-aligned) except the shared heap; 2 MB pages where
 * the RMP has them, 4 KB otherwise; pages validated at launch are left as they are */
static void sev_validate_range(uint64_t start, uint64_t end) {
    uint64_t addr = start;
    while (addr < end) {
        if (SHARED_MEM_ADDR <= addr && addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE) {
            addr = SHARED_MEM_ADDR + SHARED_MEM_SIZE;
            continue;
        }
        if (addr == g_sev_extra_shared_page) {
            addr += PAGE_SIZE;
            continue;
        }
        bool can_huge = !(addr & (0x200000UL - 1)) && end - addr >= 0x200000UL &&
                        !(addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE && SHARED_MEM_ADDR < addr + 0x200000UL) &&
                        !(g_sev_extra_shared_page >= addr && g_sev_extra_shared_page < addr + 0x200000UL);
        if (can_huge) {
            int rc = sev_pvalidate(addr, /*huge=*/true, /*validate=*/true);
            if (rc == 0 || rc == SEV_PVALIDATE_NOCHANGE) {
                g_sev_validated_pages += 512;
                addr += 0x200000UL;
                continue;
            }
            if (rc != SEV_PVALIDATE_FAIL_SIZEMISMATCH && rc != 1)
                sev_terminate(SEV_TERM_PVALIDATE);
            /* the RMP holds 4K entries here (or a part was validated at launch): go 4K */
        }
        int rc = sev_pvalidate(addr, /*huge=*/false, /*validate=*/true);
        if (rc != 0 && rc != SEV_PVALIDATE_NOCHANGE)
            sev_terminate(SEV_TERM_PVALIDATE);
        g_sev_validated_pages++;
        addr += PAGE_SIZE;
    }
}

void sev_memory_after_temp_tables(void) {
    if (!sev_snp_active())
        return;
    /* the temporary page tables map [0, 1 GB); the final ones go to [512 MB, 658 MB) */
    sev_validate_range(0, 1UL * 1024 * 1024 * 1024);
}

void sev_memory_after_final_tables(uint64_t memory_end) {
    if (!sev_snp_active())
        return;
    uint64_t t0 = get_tsc();
    if (memory_end > 1UL * 1024 * 1024 * 1024)
        sev_validate_range(1UL * 1024 * 1024 * 1024, MIN(memory_end, PCI_HOLE_ADDR));
    if (memory_end > PCI_HOLE_ADDR + PCI_HOLE_SIZE)
        sev_validate_range(PCI_HOLE_ADDR + PCI_HOLE_SIZE, memory_end);
    uint64_t t1 = get_tsc();
    if (sev_psc_range(SHARED_MEM_ADDR, SHARED_MEM_SIZE, /*shared=*/true) < 0)
        INIT_FAIL("SEV-SNP: the hypervisor refused to make the shared heap shared");
    uint64_t t2 = get_tsc();
    log_always("SEV-SNP: validated guest RAM (%lu pages up to 0x%lx, %lu Mcycles), shared heap "
               "converted to shared (%lu Mcycles)", g_sev_validated_pages, memory_end,
               (t1 - t0) / 1000000, (t2 - t1) / 1000000);
}

/* ---- per-CPU state ---- */

int sev_percpu_prepare(uint32_t num_cpus) {
    for (uint32_t i = 0; i < num_cpus; i++) {
        if (!sev_es_active())
            break;
        g_sev_vc_stacks[i] = malloc(SEV_VC_STACK_SIZE);
        if (!g_sev_vc_stacks[i])
            return -PAL_ERROR_NOMEM;
        if (i == 0)
            continue; /* CPU 0 keeps the boot GHCB */
        struct ghcb* ghcb = memory_get_shared_region(PAGE_SIZE);
        if (!ghcb)
            return -PAL_ERROR_NOMEM;
        memset(ghcb, 0, PAGE_SIZE);
        g_sev_ghcbs[i] = ghcb;
    }
    return 0;
}

uint64_t sev_vc_stack_top(uint32_t cpu_id) {
    return (uint64_t)g_sev_vc_stacks[cpu_id] + SEV_VC_STACK_SIZE;
}

/* ---- SNP AP start ---- */

static void* g_sev_ap_vmsas[MAX_NUM_CPUS];

/* RMPADJUST: set the VMSA attribute of a page (for VMPL1, the only target VMPL0 may adjust) */
static int sev_rmpadjust_vmsa(void* page, bool vmsa) {
    int rc;
    uint64_t attrs = 1 | (vmsa ? 1UL << 16 : 0); /* target VMPL 1, VMSA bit */
    __asm__ volatile(".byte 0xf3, 0x0f, 0x01, 0xfe" /* rmpadjust */
                     : "=a"(rc) : "a"(page), "c"(0), "d"(attrs) : "memory", "cc");
    return rc;
}

static void vmsa_seg(struct sev_vmsa_seg* seg, uint16_t selector, uint16_t attrib, uint32_t limit,
                     uint64_t base) {
    seg->selector = selector;
    seg->attrib = attrib;
    seg->limit = limit;
    seg->base = base;
}

int sev_snp_start_ap(uint32_t cpu_idx, void (*entry)(uint32_t)) {
    /* a VMSA must not be 2 MB aligned (SNP erratum, as Linux does): take the second of two pages */
    char* pages = malloc(3 * PAGE_SIZE);
    if (!pages)
        return -PAL_ERROR_NOMEM;
    struct sev_vmsa* vmsa = (struct sev_vmsa*)(ALIGN_UP_PTR(pages, PAGE_SIZE) + PAGE_SIZE);
    if (IS_ALIGNED_POW2((uintptr_t)vmsa, 0x200000UL))
        vmsa = (struct sev_vmsa*)((char*)vmsa - PAGE_SIZE);
    memset(vmsa, 0, PAGE_SIZE);

    void* stack = malloc(4 * PAGE_SIZE);
    if (!stack)
        return -PAL_ERROR_NOMEM;

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr, idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    uint16_t cs, ds;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    uint64_t cr0, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    uint32_t xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));

    vmsa_seg(&vmsa->cs, cs, 0xa9b, 0xffffffff, 0);  /* 64-bit code */
    vmsa_seg(&vmsa->ds, ds, 0xc93, 0xffffffff, 0);
    vmsa_seg(&vmsa->es, ds, 0xc93, 0xffffffff, 0);
    vmsa_seg(&vmsa->ss, ds, 0xc93, 0xffffffff, 0);
    vmsa_seg(&vmsa->fs, 0, 0xc93, 0xffffffff, 0);
    vmsa_seg(&vmsa->gs, 0, 0xc93, 0xffffffff, 0);
    vmsa_seg(&vmsa->gdtr, 0, 0, gdtr.limit, gdtr.base);
    vmsa_seg(&vmsa->idtr, 0, 0, 0, 0);
    vmsa_seg(&vmsa->ldtr, 0, 0x82, 0xffff, 0);
    vmsa_seg(&vmsa->tr, 0, 0x8b, 0xffff, 0);
    vmsa->efer = 0x1d01; /* SCE, LME, LMA, NXE, SVME */
    vmsa->cr0 = cr0;
    vmsa->cr3 = cr3;
    vmsa->cr4 = cr4;
    vmsa->xcr0 = ((uint64_t)xcr0_hi << 32) | xcr0_lo;
    vmsa->dr6 = 0xffff0ff0;
    vmsa->dr7 = 0x400;
    vmsa->rflags = 0x2;
    vmsa->g_pat = 0x0007040600070406UL;
    vmsa->rip = (uint64_t)entry;
    vmsa->rsp = (uint64_t)stack + 4 * PAGE_SIZE - 8;
    vmsa->rdi = cpu_idx;
    vmsa->mxcsr = 0x1f80;
    vmsa->x87_fcw = 0x37f;
    vmsa->vmpl = 0;
    vmsa->sev_features = SEV_FEATURE_SNP_ACTIVE;
    (void)idtr;

    int rc = sev_rmpadjust_vmsa(vmsa, /*vmsa=*/true);
    if (rc) {
        log_error("SEV-SNP: RMPADJUST of the AP VMSA failed (%d)", rc);
        return -PAL_ERROR_DENIED;
    }
    g_sev_ap_vmsas[cpu_idx] = vmsa;

    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    struct ghcb* ghcb = sev_this_ghcb();
    memset(ghcb->valid_bitmap, 0, sizeof(ghcb->valid_bitmap));
    GHCB_SET(ghcb, rax, vmsa->sev_features);
    rc = sev_ghcb_call(ghcb, SVM_VMGEXIT_AP_CREATION,
                       ((uint64_t)cpu_idx << 32) | SVM_VMGEXIT_AP_CREATE, (uint64_t)vmsa);
    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
    if (rc < 0) {
        log_error("SEV-SNP: AP creation of vCPU %u refused (exit info 0x%lx 0x%lx)", cpu_idx,
                  ghcb->sw_exit_info_1, ghcb->sw_exit_info_2);
        return rc;
    }
    return 0;
}

extern struct per_cpu_data* g_per_cpu_data;

void sev_ap_early(uint32_t cpu_idx) {
    /* the per-CPU data first: sev_this_ghcb() finds the vCPU through KERNEL_GS_BASE */
    uint64_t per_cpu = (uint64_t)&g_per_cpu_data[cpu_idx];
    __asm__ volatile("wrmsr" : : "c"(0xc0000102U), "a"((uint32_t)per_cpu),
                     "d"((uint32_t)(per_cpu >> 32)));
    g_per_cpu_data[cpu_idx].cpu_id = cpu_idx;
    sev_vc_early_idt_init();
    if (sev_ghcb_register(g_sev_ghcbs[cpu_idx], cpu_idx) < 0)
        sev_terminate(SEV_TERM_GHCB_REGISTER);
}
