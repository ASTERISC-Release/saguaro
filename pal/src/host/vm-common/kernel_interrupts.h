/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/* Declarations for hardware/software interrupts, Interrupt Descriptor Table and its entries.
 * Also declares Task State Segment required for interrupts happenning during ring-3 execution. */

#pragma once

#include <stdint.h>

#include "cpu.h"
#include "spinlock.h"

#define INTERRUPT_STACK_SIZE      0x4000
#define INTERRUPT_XSAVE_AREA_SIZE 0x4000 /* 16KB, should be enough for current XSAVE areas */

/* Registers for ISR pushed on the stack by hardware + `isr_x()` prologues */
struct isr_regs {
    /* pointer to FP regs (aka XSAVE area), pushed by ISRs in *.S */
    void* fpregs;

#ifdef __x86_64__
    /* 1 when _isr_common put the PAL TCB into GS over the interrupted context's own base (a
     * thread that owns %gs, kernel_events.S): the matching return loads the base back. On this
     * frame, not in the thread, so that a nested interrupt's return leaves the outer kernel
     * context alone; a return that goes through isr_iret_to_userland instead (a preempted thread,
     * a deferred upcall) carries it in thread.gs_swapped_by_isr. */
    uint64_t gs_swapped;
    /* all GPRs except those pushed by the CPU, pushed by ISRs in *.S */
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rax;
    uint64_t rcx;

    uint64_t int_number;               /* pushed by ISRs in *.S */
    uint64_t error_code;               /* pushed by the CPU or dummy by ISRs in *.S */
    uint64_t rip, cs, rflags, rsp, ss; /* pushed by the CPU (CS and SS are 16-bit values) */
#else /* __aarch64__ */
    /* All user GPRs, saved by the el0_irq asm handler: unlike x86_64 (where the preempted
     * context is a ring-3 process whose caller-saved registers the C code doesn't rely on and
     * the hardware-pushed iret frame carries rip/rflags/rsp) the aarch64 EL0 exception frame is
     * ELR_EL1/SPSR_EL1/SP_EL0 system registers, which the asm must read explicitly. The FP area
     * is again just a pointer to a per-CPU (or per-thread on the svc path) save area. */
    uint64_t regs[31]; /* x0..x30 as saved by the entry asm */
    uint64_t elr;      /* ELR_EL1: user return address (x86 `rip`) */
    uint64_t spsr;     /* SPSR_EL1: saved user PSTATE (x86 `rflags`) */
    uint64_t sp;       /* SP_EL0: user stack pointer (x86 `rsp`) */
#endif
} __attribute__((packed));

/* Interrupt Descriptor Table gate (we only use interrupt gates, no trap gates and task gates --
 * i.e., all interrupt handlers are executed with IF flag cleared aka "interrupts disabled") */
struct idt_gate {
    uint16_t isr_addr_low;  /* linear address of Interrupt Service Routine, bits 0-15 */
    uint16_t code_selector; /* set by *.S */
    uint8_t  ist_offset;    /* Interrupt Stack Table offset (always 1 meaning IST1), set by *.S */
    uint8_t  flags;         /* set by *.S to 10001110b */
    uint16_t isr_addr_mid;  /* linear address of Interrupt Service Routine, bits 16-31 */
    uint32_t isr_addr_high; /* linear address of Interrupt Service Routine, bits 32-63 */
    uint32_t _reserved;
} __attribute__((packed));

/* Interrupt Descriptor Table (currently set in *.S and unused in C) */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 64-bit-mode Task State Segment (currently set in *.S and unused in C) */
struct tss_64bitmode {
    uint32_t _reserved0;
    uint64_t rsp0_unused;
    uint64_t rsp1_unused;
    uint64_t rsp2_unused;
    uint64_t _reserved1;
    uint64_t ist1;
    uint64_t ist2_unused;
    uint64_t ist3_unused;
    uint64_t ist4_unused;
    uint64_t ist5_unused;
    uint64_t ist6_unused;
    uint64_t ist7_unused;
    uint64_t _reserved2;
    uint16_t _reserved3;
    uint16_t iomap_base_unused; /* set to 104 in *.S -- the size of this TSS struct */
} __attribute__((packed));

/* 64-bit-mode TSS segment descriptor (partially set in *.S and finished in C) */
struct tss_64bitmode_segment_descriptor {
    uint16_t limit_low_16bits;
    uint16_t base_low_16bits;
    uint8_t  base_low_8bits;
    uint8_t  p_dpl_type;
    uint8_t  flags_limit_high_8bits;
    uint8_t  base_mid_8bits;
    uint32_t base_high_32bits;
    uint32_t _reserved2;
} __attribute__((packed));

#ifdef __x86_64__
/* The GDT numbering matches Linux's (kernel_static_structs.S): user selectors 0x23 (32-bit code),
 * 0x2b (data), 0x33 (64-bit code), and the three TLS slots of set_thread_area at entries 12 to
 * 14 (selectors 0x63, 0x6b, 0x73). Each CPU has its own GDT copy (kernel_interrupts.c) because
 * the TLS slots are per thread and rewritten at every switch-in. */
#define USER_CS32_SELECTOR 0x23
#define USER_DS_SELECTOR   0x2b
#define USER_CS64_SELECTOR 0x33
#define GDT_ENTRY_TLS_MIN  12
#define GDT_ENTRIES_TLS    3
/* writes the descriptors into the TLS slots of this CPU's GDT (the caller is, or is switching to,
 * the thread that owns them) */
void gdt_load_tls(const uint64_t tls_desc[GDT_ENTRIES_TLS]);
#endif

static inline void spinlock_lock_disable_irq(spinlock_t* lock) {
    cli();
    spinlock_lock(lock);
}

#if defined(PAL_HOST_VM)
/* False until interrupts_init() on the BSP loaded the IDT: an interrupt before that (the LAPIC
 * timer armed in apic_init(), when the boot is slow) triple-faults the guest. */
extern bool g_idt_loaded;
#endif

static inline void spinlock_unlock_enable_irq(spinlock_t* lock) {
    spinlock_unlock(lock);
#if defined(PAL_HOST_VM)
    if (!g_idt_loaded)
        return; /* boot code before interrupts_init(): interrupts stay disabled */
#endif
    sti();
}

#ifdef __aarch64__
void arm_psci_system_off(void); /* arch/aarch64/kernel_multicore.c; returns only on failure */
#endif

static inline noreturn void triple_fault(void) {
#ifdef __x86_64__
    /* This IDT has a limit of 1 byte, hence #UD with this IDT loaded will result in #DF then triple
     * fault. In a microvm, a triple fault shuts down QEMU nicely. */
    static const char invalid_idtr[10] = {0};

    __asm__ volatile(
#ifdef C_NO_3F
        "hlt; jmp .\n"
#endif
        "lgdt %0; ud2\n"
        : : "m"(invalid_idtr));
#else /* __aarch64__ */
    /* No triple fault on aarch64: the equivalent of "QEMU shuts down on triple fault" is PSCI
     * SYSTEM_OFF (a no-op if the DTB has no PSCI node or it is already in flight). If that
     * returns, mask all exceptions (so even a misbehaving GIC can't get us) and spin: QEMU shows
     * this as a hung vCPU at a known address in the debugger. */
    arm_psci_system_off();
    __asm__ volatile(
#ifdef C_NO_3F
        "wfi\n"
#endif
        "msr daifset, #0xf\n"
        "1: b 1b\n"
    );
#endif
    __builtin_unreachable();
}

extern bool g_interrupts_enabled;

int pal_common_perform_memfault_handling(uint64_t faulted_addr, struct isr_regs* regs);
#ifdef __x86_64__
void pal_common_deliver_pending_interrupt(struct isr_regs* regs);
#endif

void isr_c(struct isr_regs* regs);
#ifdef __aarch64__
void isr_kernel_c(struct isr_regs* regs); /* EL1 sync-exception panic path */
#endif
int send_invalidate_tlb_ipi_and_wait(void* addr, size_t size, bool invalidate_on_this_cpu);
int interrupts_init(void);
