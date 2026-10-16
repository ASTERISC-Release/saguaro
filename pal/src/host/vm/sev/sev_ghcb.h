/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * GHCB (Guest-Hypervisor Communication Block) protocol of SEV-ES and SEV-SNP guests, AMD
 * publication 56421 ("SEV-ES Guest-Hypervisor Communication Block Standardization", version 2).
 * Under SEV-ES/SNP the hypervisor cannot read the guest's registers: every intercepted instruction
 * (CPUID, RDMSR/WRMSR, IN/OUT, MMIO, HLT, ...) raises #VC in the guest, which passes what the
 * hypervisor needs through a shared GHCB page and VMGEXIT.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MSR_AMD64_SEV_ES_GHCB 0xc0010130U

/* GHCB MSR protocol (the low 12 bits of the GHCB MSR select the function) */
#define GHCB_MSR_INFO_REQ          0x002UL
#define GHCB_MSR_INFO_RESP         0x001UL
#define GHCB_MSR_CPUID_REQ         0x004UL
#define GHCB_MSR_CPUID_RESP        0x005UL
#define GHCB_MSR_REG_GPA_REQ       0x012UL
#define GHCB_MSR_REG_GPA_RESP      0x013UL
#define GHCB_MSR_PSC_REQ           0x014UL
#define GHCB_MSR_PSC_RESP          0x015UL
#define GHCB_MSR_HV_FT_REQ         0x080UL
#define GHCB_MSR_HV_FT_RESP        0x081UL
#define GHCB_MSR_TERM_REQ          0x100UL

#define GHCB_PROTOCOL_VERSION 2

/* SVM exit codes used as GHCB "NAE event" exit codes */
#define SVM_EXIT_RDTSC         0x6eUL
#define SVM_EXIT_CPUID         0x72UL
#define SVM_EXIT_HLT           0x78UL
#define SVM_EXIT_IOIO          0x7bUL
#define SVM_EXIT_MSR           0x7cUL
#define SVM_EXIT_VMMCALL       0x81UL
#define SVM_EXIT_RDTSCP        0x87UL
#define SVM_EXIT_WBINVD        0x89UL
#define SVM_EXIT_MONITOR       0x8aUL
#define SVM_EXIT_MWAIT         0x8bUL
#define SVM_EXIT_XSETBV        0x8dUL
#define SVM_EXIT_NPF           0x400UL
#define SVM_VMGEXIT_MMIO_READ  0x80000001UL
#define SVM_VMGEXIT_MMIO_WRITE 0x80000002UL
#define SVM_VMGEXIT_PSC        0x80000010UL
#define SVM_VMGEXIT_GUEST_REQUEST     0x80000011UL
#define SVM_VMGEXIT_EXT_GUEST_REQUEST 0x80000012UL
#define SVM_VMGEXIT_AP_CREATION       0x80000013UL
#define SVM_VMGEXIT_AP_CREATE         1UL
#define SVM_VMGEXIT_AP_DESTROY        2UL
#define SVM_VMGEXIT_HV_FEATURES       0x8000fffdUL

/* IOIO exit information (APM vol. 2, 15.10.2) */
#define IOIO_TYPE_IN   (1UL << 0)
#define IOIO_STR       (1UL << 2)
#define IOIO_REP       (1UL << 3)
#define IOIO_DATA_8    (1UL << 4)
#define IOIO_DATA_16   (1UL << 5)
#define IOIO_DATA_32   (1UL << 6)
#define IOIO_ADDR_16   (1UL << 7)
#define IOIO_ADDR_32   (1UL << 8)
#define IOIO_ADDR_64   (1UL << 9)

/* The GHCB page (offsets of the save area are those of the VMSA) */
struct ghcb {
    uint8_t  reserved_0x0[0xcb];
    uint8_t  cpl;                    /* 0x0cb */
    uint8_t  reserved_0xcc[0x74];
    uint64_t xss;                    /* 0x140 */
    uint8_t  reserved_0x148[0x18];
    uint64_t dr7;                    /* 0x160 */
    uint8_t  reserved_0x168[0x10];
    uint64_t rip;                    /* 0x178 */
    uint8_t  reserved_0x180[0x58];
    uint64_t rsp;                    /* 0x1d8 */
    uint8_t  reserved_0x1e0[0x18];
    uint64_t rax;                    /* 0x1f8 */
    uint8_t  reserved_0x200[0x108];
    uint64_t rcx;                    /* 0x308 */
    uint64_t rdx;                    /* 0x310 */
    uint64_t rbx;                    /* 0x318 */
    uint8_t  reserved_0x320[0x8];
    uint64_t rbp;                    /* 0x328 */
    uint64_t rsi;                    /* 0x330 */
    uint64_t rdi;                    /* 0x338 */
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15; /* 0x340 */
    uint8_t  reserved_0x380[0x10];
    uint64_t sw_exit_code;           /* 0x390 */
    uint64_t sw_exit_info_1;         /* 0x398 */
    uint64_t sw_exit_info_2;         /* 0x3a0 */
    uint64_t sw_scratch;             /* 0x3a8 */
    uint8_t  reserved_0x3b0[0x38];
    uint64_t xcr0;                   /* 0x3e8 */
    uint8_t  valid_bitmap[0x10];     /* 0x3f0 */
    uint64_t x87_state_gpa;          /* 0x400 */
    uint8_t  reserved_0x408[0x3f8];
    uint8_t  shared_buffer[0x7f0];   /* 0x800 */
    uint8_t  reserved_0xff0[0xa];
    uint16_t protocol_version;       /* 0xffa */
    uint32_t ghcb_usage;             /* 0xffc */
} __attribute__((packed));

#define GHCB_SHARED_BUFFER_SIZE 0x7f0

_Static_assert(sizeof(struct ghcb) == 4096, "GHCB must be one page");
_Static_assert(offsetof(struct ghcb, sw_exit_code) == 0x390, "GHCB layout");
_Static_assert(offsetof(struct ghcb, shared_buffer) == 0x800, "GHCB layout");

/* the valid bitmap has one bit per 8-byte field of the save area */
#define GHCB_FIELD(field) (offsetof(struct ghcb, field) / 8)

static inline void ghcb_set_valid(struct ghcb* ghcb, size_t field_index) {
    ghcb->valid_bitmap[field_index / 8] |= 1 << (field_index % 8);
}

static inline bool ghcb_is_valid(struct ghcb* ghcb, size_t field_index) {
    return ghcb->valid_bitmap[field_index / 8] & (1 << (field_index % 8));
}

#define GHCB_SET(ghcb, field, value)                       \
    do {                                                   \
        (ghcb)->field = (value);                           \
        ghcb_set_valid((ghcb), GHCB_FIELD(field));         \
    } while (0)

static inline uint64_t ghcb_msr_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_AMD64_SEV_ES_GHCB));
    return ((uint64_t)hi << 32) | lo;
}

static inline void ghcb_msr_write(uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(MSR_AMD64_SEV_ES_GHCB), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)) : "memory");
}

static inline void vmgexit(void) {
    __asm__ volatile("rep; vmmcall" : : : "memory");
}

/* One MSR-protocol exchange: write `request`, VMGEXIT, return the GHCB MSR (the caller restores
 * the GHCB address if one is registered). */
static inline uint64_t ghcb_msr_protocol(uint64_t request) {
    ghcb_msr_write(request);
    vmgexit();
    return ghcb_msr_read();
}

/* The GHCB of this vCPU (registered, shared), or NULL before sev_ghcb_init(). */
struct ghcb* sev_this_ghcb(void);

/* Initializes the boot vCPU's GHCB (a page of the PAL's .bss made shared); ES/SNP only. */
int sev_ghcb_init_boot(void);
/* Registers `ghcb` (a shared page) as the GHCB of the calling vCPU `cpu_id`. */
int sev_ghcb_register(struct ghcb* ghcb, uint32_t cpu_id);
extern struct ghcb* g_sev_ghcbs[];

/* Performs a NAE event through `ghcb` (already filled except exit code/infos). Returns 0, or
 * -PAL_ERROR_DENIED if the hypervisor reported an error. Interrupts must be off. */
int sev_ghcb_call(struct ghcb* ghcb, uint64_t exit_code, uint64_t info1, uint64_t info2);

/* Terminates the guest through the MSR protocol (reason set 0x47 'G', code). */
__attribute__((noreturn)) void sev_terminate(uint32_t code);

/* GHCB-based host operations (ES/SNP), usable with interrupts on or off */
uint64_t sev_ghcb_rdmsr(uint32_t msr);
void sev_ghcb_wrmsr(uint32_t msr, uint64_t value);
uint32_t sev_ghcb_ioio_in(uint16_t port, unsigned size);
void sev_ghcb_ioio_out(uint16_t port, unsigned size, uint32_t value);
uint64_t sev_ghcb_mmio_read(uint64_t gpa, unsigned size);
void sev_ghcb_mmio_write(uint64_t gpa, unsigned size, uint64_t value);
void sev_ghcb_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);
void sev_ghcb_hlt(void);
