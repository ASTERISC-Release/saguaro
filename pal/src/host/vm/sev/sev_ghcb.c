/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * GHCB calls of SEV-ES/SNP guests (sev_ghcb.h). This file is built with -mgeneral-regs-only and
 * without the stack protector (sev/meson.build): it runs in the #VC handler, which may interrupt
 * user code and must not touch the FPU/SSE state or %gs.
 */

#include "api.h"
#include "pal_error.h"

#include "kernel_sched.h"
#include "sev.h"
#include "sev_ghcb.h"

uint64_t g_sev_extra_shared_page;

/* the boot vCPU's GHCB, a page of .bss converted to shared in sev_ghcb_init_boot() (kept for
 * CPU 0); the other vCPUs get theirs from the shared heap (sev_snp.c) */
struct ghcb g_sev_boot_ghcb __attribute__((aligned(4096)));

/* GHCB of each vCPU, indexed by cpu_id; before the per-CPU data exists only [0] is used */
struct ghcb* g_sev_ghcbs[MAX_NUM_CPUS];

uint32_t sev_this_cpu_id(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000102U)); /* KERNEL_GS_BASE */
    uint64_t per_cpu = ((uint64_t)hi << 32) | lo;
    if (!per_cpu)
        return 0;
    return *(uint32_t*)per_cpu; /* struct per_cpu_data::cpu_id is the first field */
}

struct ghcb* sev_this_ghcb(void) {
    return g_sev_ghcbs[sev_this_cpu_id()];
}

__attribute__((noreturn)) void sev_terminate(uint32_t code) {
    /* reason set 0x47 ('G'ramine) in bits 15:12 is not standard: use set 0 (general), code */
    uint64_t req = GHCB_MSR_TERM_REQ | (0UL << 12) | ((uint64_t)(code & 0xff) << 16);
    for (;;) {
        ghcb_msr_write(req);
        vmgexit();
        __asm__ volatile("hlt");
    }
}

static inline void sev_zero_page(void* page) {
    uint64_t* p = page;
    for (size_t i = 0; i < 4096 / sizeof(uint64_t); i++)
        p[i] = 0;
}

int sev_ghcb_register(struct ghcb* ghcb, uint32_t cpu_id) {
    uint64_t gpa = (uint64_t)ghcb; /* identity-mapped */
    uint64_t resp = ghcb_msr_protocol(GHCB_MSR_REG_GPA_REQ | (gpa & ~0xfffUL));
    if ((resp & 0xfff) != GHCB_MSR_REG_GPA_RESP || (resp & ~0xfffUL) != (gpa & ~0xfffUL))
        return -PAL_ERROR_DENIED;
    ghcb_msr_write(gpa);
    g_sev_ghcbs[cpu_id] = ghcb;
    return 0;
}

static inline void ghcb_clear(struct ghcb* ghcb) {
    /* the valid bitmap and exit fields are what the hypervisor looks at; clear the bitmap so that
     * no stale field of an earlier call is taken as an input */
    for (size_t i = 0; i < sizeof(ghcb->valid_bitmap); i++)
        ghcb->valid_bitmap[i] = 0;
    ghcb->protocol_version = GHCB_PROTOCOL_VERSION;
    ghcb->ghcb_usage = 0;
}

int sev_ghcb_call(struct ghcb* ghcb, uint64_t exit_code, uint64_t info1, uint64_t info2) {
    GHCB_SET(ghcb, sw_exit_code, exit_code);
    GHCB_SET(ghcb, sw_exit_info_1, info1);
    GHCB_SET(ghcb, sw_exit_info_2, info2);
    ghcb->protocol_version = GHCB_PROTOCOL_VERSION;
    ghcb->ghcb_usage = 0;
    ghcb_msr_write((uint64_t)ghcb);
    vmgexit();
    /* low 32 bits of sw_exit_info_1: 0 = ok, 1 = error (info2 holds an exception to inject),
     * 2 = the request was malformed */
    if ((uint32_t)ghcb->sw_exit_info_1 != 0)
        return -PAL_ERROR_DENIED;
    return 0;
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
}

/* The explicit calls below run with interrupts off so that an interrupt handler's #VC, which uses
 * the same per-CPU GHCB, cannot overwrite the block in the middle of a call. */

uint64_t sev_ghcb_rdmsr(uint32_t msr) {
    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    GHCB_SET(ghcb, rcx, msr);
    if (sev_ghcb_call(ghcb, SVM_EXIT_MSR, /*read=*/0, 0) < 0)
        sev_terminate(SEV_TERM_GHCB_CALL);
    uint64_t value = (ghcb->rax & 0xffffffffUL) | (ghcb->rdx << 32);
    irq_restore(flags);
    return value;
}

void sev_ghcb_wrmsr(uint32_t msr, uint64_t value) {
    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    GHCB_SET(ghcb, rcx, msr);
    GHCB_SET(ghcb, rax, value & 0xffffffffUL);
    GHCB_SET(ghcb, rdx, value >> 32);
    if (sev_ghcb_call(ghcb, SVM_EXIT_MSR, /*write=*/1, 0) < 0)
        sev_terminate(SEV_TERM_GHCB_CALL);
    irq_restore(flags);
}

static uint64_t ioio_info(uint16_t port, unsigned size, bool in) {
    uint64_t info = ((uint64_t)port << 16) | IOIO_ADDR_16;
    info |= size == 1 ? IOIO_DATA_8 : size == 2 ? IOIO_DATA_16 : IOIO_DATA_32;
    if (in)
        info |= IOIO_TYPE_IN;
    return info;
}

uint32_t sev_ghcb_ioio_in(uint16_t port, unsigned size) {
    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    GHCB_SET(ghcb, rax, 0);
    if (sev_ghcb_call(ghcb, SVM_EXIT_IOIO, ioio_info(port, size, /*in=*/true), 0) < 0)
        sev_terminate(SEV_TERM_GHCB_CALL);
    uint32_t value = (uint32_t)ghcb->rax;
    irq_restore(flags);
    if (size == 1)
        return value & 0xff;
    if (size == 2)
        return value & 0xffff;
    return value;
}

void sev_ghcb_ioio_out(uint16_t port, unsigned size, uint32_t value) {
    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    GHCB_SET(ghcb, rax, value);
    if (sev_ghcb_call(ghcb, SVM_EXIT_IOIO, ioio_info(port, size, /*in=*/false), 0) < 0)
        sev_terminate(SEV_TERM_GHCB_CALL);
    irq_restore(flags);
}

uint64_t sev_ghcb_mmio_read(uint64_t gpa, unsigned size) {
    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    GHCB_SET(ghcb, sw_scratch, (uint64_t)ghcb->shared_buffer);
    uint8_t* buf = ghcb->shared_buffer;
    for (unsigned i = 0; i < 8; i++)
        buf[i] = 0;
    if (sev_ghcb_call(ghcb, SVM_VMGEXIT_MMIO_READ, gpa, size) < 0)
        sev_terminate(SEV_TERM_GHCB_CALL);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++)
        value |= (uint64_t)buf[i] << (8 * i);
    irq_restore(flags);
    return value;
}

void sev_ghcb_mmio_write(uint64_t gpa, unsigned size, uint64_t value) {
    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    GHCB_SET(ghcb, sw_scratch, (uint64_t)ghcb->shared_buffer);
    uint8_t* buf = ghcb->shared_buffer;
    for (unsigned i = 0; i < size; i++)
        buf[i] = (uint8_t)(value >> (8 * i));
    if (sev_ghcb_call(ghcb, SVM_VMGEXIT_MMIO_WRITE, gpa, size) < 0)
        sev_terminate(SEV_TERM_GHCB_CALL);
    irq_restore(flags);
}

void sev_ghcb_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    GHCB_SET(ghcb, rax, leaf);
    GHCB_SET(ghcb, rcx, subleaf);
    if (leaf == 0xd) {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        GHCB_SET(ghcb, xcr0, ((uint64_t)hi << 32) | lo);
    }
    if (sev_ghcb_call(ghcb, SVM_EXIT_CPUID, 0, 0) < 0)
        sev_terminate(SEV_TERM_GHCB_CALL);
    out[0] = (uint32_t)ghcb->rax;
    out[1] = (uint32_t)ghcb->rbx;
    out[2] = (uint32_t)ghcb->rcx;
    out[3] = (uint32_t)ghcb->rdx;
    irq_restore(flags);
}

void sev_ghcb_hlt(void) {
    struct ghcb* ghcb = sev_this_ghcb();
    ghcb_clear(ghcb);
    sev_ghcb_call(ghcb, SVM_EXIT_HLT, 0, 0);
}

/* PVALIDATE: (re)validate or rescind one guest page; returns the instruction's return code, or
 * SEV_PVALIDATE_NOCHANGE if the page already was in the requested state (CF=1) */
int sev_pvalidate(uint64_t va, bool huge, bool validate) {
    int rc;
    uint8_t no_change;
    __asm__ volatile(".byte 0xf2, 0x0f, 0x01, 0xff\n\t" /* pvalidate */
                     "setc %1"
                     : "=a"(rc), "=r"(no_change)
                     : "a"(va), "c"(huge ? 1 : 0), "d"(validate ? 1 : 0)
                     : "memory", "cc");
    if (rc == 0 && no_change)
        return SEV_PVALIDATE_NOCHANGE;
    return rc;
}

/* Page-state change of one 4K page through the MSR protocol (usable before a GHCB exists) */
int sev_psc_msr(uint64_t gpa, bool shared) {
    uint64_t req = GHCB_MSR_PSC_REQ | (gpa & 0x000ffffffffff000UL) |
                   ((shared ? 2UL : 1UL) << 52);
    uint64_t saved = ghcb_msr_read();
    uint64_t resp = ghcb_msr_protocol(req);
    ghcb_msr_write(saved);
    if ((resp & 0xfff) != GHCB_MSR_PSC_RESP || (resp >> 32))
        return -PAL_ERROR_DENIED;
    return 0;
}

/* Page-state change of [gpa, gpa + size) through the GHCB (2 MB entries where aligned) */
int sev_psc_range(uint64_t gpa, uint64_t size, bool shared) {
    struct psc_hdr {
        uint16_t cur_entry;
        uint16_t end_entry;
        uint32_t reserved;
    };
    const size_t max_entries = (GHCB_SHARED_BUFFER_SIZE - sizeof(struct psc_hdr)) / 8;
    uint64_t end = gpa + size;

    uint64_t flags = irq_save();
    struct ghcb* ghcb = sev_this_ghcb();
    while (gpa < end) {
        struct psc_hdr* hdr = (struct psc_hdr*)ghcb->shared_buffer;
        uint64_t* entries = (uint64_t*)(ghcb->shared_buffer + sizeof(*hdr));
        size_t n = 0;
        while (gpa < end && n < max_entries) {
            bool huge = !(gpa & 0x1fffffUL) && end - gpa >= 0x200000UL;
            entries[n++] = (gpa & 0x000ffffffffff000UL) | ((shared ? 2UL : 1UL) << 52) |
                           (huge ? 1UL << 56 : 0);
            gpa += huge ? 0x200000UL : 0x1000UL;
        }
        hdr->cur_entry = 0;
        hdr->end_entry = (uint16_t)(n - 1);
        hdr->reserved = 0;
        /* the hypervisor may process only part of the list per call: repeat until done */
        for (int tries = 0; hdr->cur_entry <= hdr->end_entry; tries++) {
            ghcb_clear(ghcb);
            GHCB_SET(ghcb, sw_scratch, (uint64_t)ghcb->shared_buffer);
            if (tries > 1000 || sev_ghcb_call(ghcb, SVM_VMGEXIT_PSC, 0, 0) < 0 ||
                    ghcb->sw_exit_info_2) {
                irq_restore(flags);
                return -PAL_ERROR_DENIED;
            }
        }
    }
    irq_restore(flags);
    return 0;
}

int sev_ghcb_init_boot(void) {
    struct ghcb* ghcb = &g_sev_boot_ghcb;
    uint64_t gpa = (uint64_t)ghcb;

    /* negotiate the protocol version: the hypervisor must support version 2 */
    uint64_t resp = ghcb_msr_protocol(GHCB_MSR_INFO_REQ);
    if ((resp & 0xfff) != GHCB_MSR_INFO_RESP)
        sev_terminate(SEV_TERM_GHCB_PROTOCOL);
    uint16_t max = (uint16_t)(resp >> 48), min = (uint16_t)(resp >> 32);
    if (min > GHCB_PROTOCOL_VERSION || max < GHCB_PROTOCOL_VERSION)
        sev_terminate(SEV_TERM_GHCB_PROTOCOL);

    if (g_sev_status & SEV_STATUS_SEV_SNP) {
        /* the page was validated (private) at launch: rescind, make it shared */
        int rc = sev_pvalidate(gpa, /*huge=*/false, /*validate=*/false);
        if (rc != 0 && rc != SEV_PVALIDATE_NOCHANGE)
            sev_terminate(SEV_TERM_PVALIDATE);
        if (sev_psc_msr(gpa, /*shared=*/true) < 0)
            sev_terminate(SEV_TERM_PSC);
    }
    /* map it unencrypted (the boot page tables of vm_bootloader.S cover it with 4K entries) */
    sev_boot_pte_set_shared(gpa);
    sev_zero_page(ghcb);

    if (sev_ghcb_register(ghcb, /*cpu_id=*/0) < 0)
        sev_terminate(SEV_TERM_GHCB_REGISTER);
    g_sev_extra_shared_page = gpa;
    return 0;
}

/* the 4K PTEs of the static boot page tables (vm_bootloader.S), which map [0, 32 MB) */
extern uint64_t _initial_page_tables[];

void sev_boot_pte_set_shared(uint64_t gpa) {
    _initial_page_tables[gpa >> 12] &= ~g_sev_c_bit_mask;
    __asm__ volatile("invlpg (%0)" : : "r"(gpa) : "memory");
    /* lines of the page cached as private must not be written back over the shared data later:
     * the page is not written before this, but flush for completeness (WBINVD would #VC) */
    for (uint64_t line = gpa; line < gpa + 4096; line += 64)
        __asm__ volatile("clflush (%0)" : : "r"(line) : "memory");
}
