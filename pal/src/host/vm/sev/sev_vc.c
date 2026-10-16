/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * #VC (VMM Communication exception, vector 29) handler of SEV-ES/SNP guests. Under SEV-ES and SNP
 * the hypervisor cannot see the guest's registers, so an intercepted instruction raises #VC in the
 * guest instead of exiting; this handler decodes the instruction, forwards it through the GHCB and
 * emulates its effect.
 *
 * Built with -mgeneral-regs-only and without the stack protector (sev/meson.build): the handler
 * may interrupt any code, user mode included, and saves no FPU state; it must not raise #VC
 * itself (it uses only GHCB calls).
 *
 * CPUID under SNP comes from the CPUID page that the SNP firmware checked at launch (not from the
 * hypervisor), except the hypervisor leaves 0x40000000.., which the page does not describe.
 */

#include "api.h"
#include "pal_error.h"

#include "kernel_interrupts.h"
#include "sev.h"
#include "sev_ghcb.h"

/* SNP CPUID page (SEV-SNP firmware ABI, "CPUID function structure") */
struct snp_cpuid_fn {
    uint32_t eax_in;
    uint32_t ecx_in;
    uint64_t xcr0_in;
    uint64_t xss_in;
    uint32_t eax, ebx, ecx, edx;
    uint64_t reserved;
} __attribute__((packed));

struct snp_cpuid_table {
    uint32_t count;
    uint32_t reserved1;
    uint64_t reserved2;
    struct snp_cpuid_fn fn[64];
} __attribute__((packed));

_Static_assert(sizeof(struct snp_cpuid_table) <= 4096, "CPUID page");

/* leaves whose output depends on the subleaf (ECX) */
static bool cpuid_leaf_has_subleaves(uint32_t leaf) {
    switch (leaf) {
        case 0x4: case 0x7: case 0xb: case 0xd: case 0xf: case 0x10: case 0x12: case 0x14:
        case 0x17: case 0x18: case 0x1d: case 0x1f: case 0x20: case 0x23: case 0x24:
        case 0x8000001d: case 0x80000020: case 0x80000026:
            return true;
        default:
            return false;
    }
}

static const struct snp_cpuid_fn* snp_cpuid_find(uint32_t leaf, uint32_t subleaf) {
    const struct snp_cpuid_table* table = (const struct snp_cpuid_table*)SEV_SNP_CPUID_PAGE_GPA;
    uint32_t count = table->count;
    if (count > 64)
        count = 64;
    bool sub = cpuid_leaf_has_subleaves(leaf);
    for (uint32_t i = 0; i < count; i++) {
        const struct snp_cpuid_fn* fn = &table->fn[i];
        if (fn->eax_in == leaf && (!sub || fn->ecx_in == subleaf))
            return fn;
    }
    return NULL;
}

static uint64_t read_cr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static uint64_t read_xcr0(void) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

/* XSAVE area size for the state components in `xfeatures` (standard format), from the
 * per-component sizes and offsets of leaf 0xD of the CPUID page */
static uint32_t xsave_size(uint64_t xfeatures, bool compacted) {
    uint32_t size = 576; /* legacy area + header */
    for (uint32_t i = 2; i < 64; i++) {
        if (!(xfeatures & (1UL << i)))
            continue;
        const struct snp_cpuid_fn* fn = snp_cpuid_find(0xd, i);
        if (!fn)
            continue;
        if (compacted)
            size += fn->eax;
        else if (fn->ebx + fn->eax > size)
            size = fn->ebx + fn->eax;
    }
    return size;
}

void sev_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
    if (!(g_sev_status & SEV_STATUS_SEV_SNP)) {
        sev_ghcb_cpuid(leaf, subleaf, out);
        return;
    }
    if (leaf >= 0x40000000 && leaf < 0x40010000) {
        /* hypervisor leaves (KVM's TSC frequency, ...): only the hypervisor knows them; they are
         * as untrusted as without SEV (Secure TSC is not used) */
        sev_ghcb_cpuid(leaf, subleaf, out);
        return;
    }
    const struct snp_cpuid_fn* fn = snp_cpuid_find(leaf, subleaf);
    if (!fn) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = fn->eax;
    out[1] = fn->ebx;
    out[2] = fn->ecx;
    out[3] = fn->edx;

    /* the values that depend on the vCPU or its current state */
    switch (leaf) {
        case 0x1:
            out[1] = (out[1] & 0x00ffffffU) | (sev_this_cpu_id() << 24); /* initial APIC ID */
            if (read_cr4() & (1UL << 18))
                out[2] |= 1U << 27; /* OSXSAVE */
            else
                out[2] &= ~(1U << 27);
            break;
        case 0x7:
            if (subleaf == 0) {
                if (read_cr4() & (1UL << 22))
                    out[2] |= 1U << 4; /* OSPKE */
                else
                    out[2] &= ~(1U << 4);
            }
            break;
        case 0xb:
        case 0x1f:
            out[3] = sev_this_cpu_id(); /* x2APIC ID */
            break;
        case 0xd:
            if (subleaf == 0 && (read_cr4() & (1UL << 18)))
                out[1] = xsave_size(read_xcr0(), /*compacted=*/false);
            else if (subleaf == 1)
                out[1] = xsave_size(read_xcr0(), /*compacted=*/true);
            break;
        case 0x8000001e:
            out[0] = sev_this_cpu_id(); /* extended APIC ID */
            break;
    }
}

/* ---- instruction decoding ---- */

struct insn {
    uint8_t opsize_16;  /* 0x66 prefix */
    uint8_t rep;        /* 0xf3 prefix */
    uint8_t len;
    const uint8_t* op;  /* first opcode byte after the prefixes */
};

static bool decode_prefixes(struct isr_regs* regs, struct insn* insn) {
    const uint8_t* p = (const uint8_t*)regs->rip;
    insn->opsize_16 = 0;
    insn->rep = 0;
    for (unsigned i = 0; i < 4; i++, p++) {
        if (*p == 0x66)
            insn->opsize_16 = 1;
        else if (*p == 0xf3)
            insn->rep = 1;
        else if ((*p & 0xf0) == 0x40)
            continue; /* REX (no meaning for the instructions decoded here) */
        else
            break;
    }
    insn->op = p;
    insn->len = (uint8_t)(p - (const uint8_t*)regs->rip);
    return true;
}

static void set_rax_size(struct isr_regs* regs, unsigned size, uint64_t value) {
    if (size == 1)
        regs->rax = (regs->rax & ~0xffUL) | (value & 0xff);
    else if (size == 2)
        regs->rax = (regs->rax & ~0xffffUL) | (value & 0xffff);
    else
        regs->rax = value & 0xffffffffUL; /* 32-bit results zero-extend */
}

static int vc_ioio(struct isr_regs* regs, struct insn* insn) {
    uint8_t op = insn->op[0];
    unsigned size;
    uint16_t port;
    bool in;
    unsigned len = insn->len + 1;
    switch (op) {
        case 0xe4: case 0xe5: case 0xe6: case 0xe7:
            port = insn->op[1];
            len++;
            break;
        case 0xec: case 0xed: case 0xee: case 0xef:
            port = (uint16_t)regs->rdx;
            break;
        default:
            return -PAL_ERROR_NOTIMPLEMENTED; /* INS/OUTS are not used by the PAL */
    }
    in = (op & 0x2) == 0;
    size = (op & 0x1) ? (insn->opsize_16 ? 2 : 4) : 1;
    if (in)
        set_rax_size(regs, size, sev_ghcb_ioio_in(port, size));
    else
        sev_ghcb_ioio_out(port, size, (uint32_t)regs->rax);
    regs->rip += len;
    return 0;
}

int sev_vc_handler(struct isr_regs* regs) {
    struct insn insn;
    uint64_t exit_code = regs->error_code;
    decode_prefixes(regs, &insn);
    const uint8_t* op = insn.op;

    switch (exit_code) {
        case SVM_EXIT_CPUID: {
            if (op[0] != 0x0f || op[1] != 0xa2)
                return -PAL_ERROR_INVAL;
            uint32_t out[4];
            sev_cpuid((uint32_t)regs->rax, (uint32_t)regs->rcx, out);
            regs->rax = out[0];
            regs->rbx = out[1];
            regs->rcx = out[2];
            regs->rdx = out[3];
            regs->rip += insn.len + 2;
            return 0;
        }
        case SVM_EXIT_MSR: {
            if (op[0] != 0x0f || (op[1] != 0x30 && op[1] != 0x32))
                return -PAL_ERROR_INVAL;
            uint32_t msr = (uint32_t)regs->rcx;
            if (op[1] == 0x32) {
                uint64_t value = sev_ghcb_rdmsr(msr);
                regs->rax = value & 0xffffffffUL;
                regs->rdx = value >> 32;
            } else {
                sev_ghcb_wrmsr(msr, (regs->rax & 0xffffffffUL) | (regs->rdx << 32));
            }
            regs->rip += insn.len + 2;
            return 0;
        }
        case SVM_EXIT_IOIO:
            return vc_ioio(regs, &insn);
        case SVM_EXIT_XSETBV: {
            if (op[0] != 0x0f || op[1] != 0x01 || op[2] != 0xd1)
                return -PAL_ERROR_INVAL;
            struct ghcb* ghcb = sev_this_ghcb();
            for (size_t i = 0; i < sizeof(ghcb->valid_bitmap); i++)
                ghcb->valid_bitmap[i] = 0;
            GHCB_SET(ghcb, rcx, regs->rcx & 0xffffffffUL);
            GHCB_SET(ghcb, rax, regs->rax & 0xffffffffUL);
            GHCB_SET(ghcb, rdx, regs->rdx & 0xffffffffUL);
            GHCB_SET(ghcb, cpl, (uint8_t)(regs->cs & 3));
            if (sev_ghcb_call(ghcb, SVM_EXIT_XSETBV, 0, 0) < 0)
                return -PAL_ERROR_DENIED;
            regs->rip += insn.len + 3;
            return 0;
        }
        case SVM_EXIT_WBINVD: {
            struct ghcb* ghcb = sev_this_ghcb();
            for (size_t i = 0; i < sizeof(ghcb->valid_bitmap); i++)
                ghcb->valid_bitmap[i] = 0;
            if (sev_ghcb_call(ghcb, SVM_EXIT_WBINVD, 0, 0) < 0)
                return -PAL_ERROR_DENIED;
            regs->rip += insn.len + 2;
            return 0;
        }
        case SVM_EXIT_RDTSC:
        case SVM_EXIT_RDTSCP: {
            /* not intercepted by KVM; handled for completeness through the hypervisor */
            struct ghcb* ghcb = sev_this_ghcb();
            for (size_t i = 0; i < sizeof(ghcb->valid_bitmap); i++)
                ghcb->valid_bitmap[i] = 0;
            if (sev_ghcb_call(ghcb, exit_code, 0, 0) < 0)
                return -PAL_ERROR_DENIED;
            regs->rax = ghcb->rax & 0xffffffffUL;
            regs->rdx = ghcb->rdx & 0xffffffffUL;
            if (exit_code == SVM_EXIT_RDTSCP)
                regs->rcx = ghcb->rcx & 0xffffffffUL;
            regs->rip += insn.len + (exit_code == SVM_EXIT_RDTSCP ? 3 : 2);
            return 0;
        }
        default:
            return -PAL_ERROR_NOTIMPLEMENTED;
    }
}

/* ---- entry from sev_vc_entry.S ---- */

#define SERIAL_PORT 0x3f8

static void vc_puts(const char* s) {
    for (; *s; s++)
        sev_ghcb_ioio_out(SERIAL_PORT, 1, (uint8_t)*s);
}

static void vc_puthex(uint64_t v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned d = (v >> (60 - 4 * i)) & 0xf;
        buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[18] = 0;
    vc_puts(buf);
}

void sev_vc_entry_c(struct isr_regs* regs) {
    int ret = sev_vc_handler(regs);
    if (ret == 0)
        return;
    vc_puts("SEV: unhandled #VC, exit code ");
    vc_puthex(regs->error_code);
    vc_puts(" rip ");
    vc_puthex(regs->rip);
    vc_puts(" cs ");
    vc_puthex(regs->cs);
    vc_puts(" bytes ");
    vc_puthex(*(const uint64_t*)regs->rip);
    vc_puts("\n");
    sev_terminate(SEV_TERM_VC_UNHANDLED);
}
