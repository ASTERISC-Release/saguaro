/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Hardware/software interrupts. See also the bootloader code.
 *
 * Notes on multi-core synchronization:
 *   - processing different interrupt numbers in isr_c() requires different sync techniques, see
 *     comments in that func for more details
 *   - all other funcs are used only at init, no sync required
 */

#include <stdint.h>

#include "api.h"
#include "cpu.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_apic.h"
#include "kernel_interrupts.h"
#include "pal_common.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_pci.h"
#include "kernel_sched.h"
#include "kernel_syscalls.h"
#include "kernel_time.h"
#include "kernel_virtio.h"
#include "vm_callbacks.h"
#ifdef PAL_HOST_SEV
#include "sev.h"
#endif

/* Interrupt Service Routines (ISRs) are defined in the *.S */
extern void isr_0(void);
extern void isr_1(void);
extern void isr_2(void);
extern void isr_3(void);
extern void isr_4(void);
extern void isr_5(void);
extern void isr_6(void);
extern void isr_7(void);
extern void isr_8(void);
extern void isr_9(void);
extern void isr_10(void);
extern void isr_11(void);
extern void isr_12(void);
extern void isr_13(void);
extern void isr_14(void);
extern void isr_15(void);
extern void isr_16(void);
extern void isr_17(void);
extern void isr_18(void);
extern void isr_19(void);
extern void isr_20(void);
extern void isr_32(void);
extern void isr_33(void);
extern void isr_64(void);
extern void isr_spurious(void);

/* can be accessed without atomics as there is only one CPU (BSP) that modifies it (at boot time),
 * and afterwards it is read-only */
bool g_interrupts_enabled = false;

#ifdef PAL_HOST_VM
bool g_idt_loaded = false;
#endif

static struct idt_gate* g_idt = NULL; /* IDT with 256 partially filled gates, see *.S */

/* A single "invalidate TLB" structure used when page table entries (PTEs) are updated by one vCPU.
 * The vCPU populates this structure with mem region to invalidate, invalidates this region itself,
 * sets `num_responses = 1` and sends an "invalidate TLB" IPI to other vCPUs via the LAPIC ICR.
 * We have a single object protected by a single lock, so if two vCPUs must update PTEs, there is a
 * performance hit of doing the two operations one after another. Also note that the whole
 * "invalidate TLB" IPI protocol must be secure against missing/spurious/extra IPIs, as ICR is
 * controlled by the untrusted host. */
struct invalidate_tlb_request_t {
    void* addr;
    size_t size;
    uint32_t num_responses; /* must be accessed atomically in interrupt handler of each vCPU */
    bool in_progress;       /* must be accessed atomically in interrupt handler of each vCPU */
};

static struct invalidate_tlb_request_t g_invalidate_tlb_request;
static spinlock_t g_invalidate_tlb_request_lock = INIT_SPINLOCK_UNLOCKED;

void isr_c(struct isr_regs* regs) {
    int ret;

    extern char gdt_start[1];
    extern char gdt_entry_kernel_cs[1];
    uint64_t kernel_cs = (uint64_t)(gdt_entry_kernel_cs - gdt_start);

    switch (regs->int_number) {
        case 0:  /* #DE, divide error */
        case 16: /* #MF, x87 floating-point error */
        case 19: /* #XM, SIMD floating-point error */
            /* SIGFPE for the application (Wine installs a handler; its si_addr is the instruction).
             * In the PAL's own code it stays a panic, as before. */
            ret = pal_common_perform_event_handling(PAL_EVENT_ARITHMETIC_ERROR, regs->rip, regs);
            if (ret < 0) {
                log_error("Panic: arithmetic exception (vector nr. %lu) not handled (rip=0x%lx "
                          "rsp=0x%lx)", regs->int_number, regs->rip, regs->rsp);
                triple_fault();
            }
            break;
        case 6:  /* #UD, invalid opcode */
            /* SIGILL for the application (ud2, an instruction the CPU lacks, Wine's probes) */
            ret = pal_common_perform_event_handling(PAL_EVENT_ILLEGAL, regs->rip, regs);
            if (ret < 0) {
                log_error("Panic: invalid opcode not handled (rip=0x%lx rsp=0x%lx)", regs->rip,
                          regs->rsp);
                triple_fault();
            }
            break;
        case 13:
            /* `int $0x80` from user mode (no IDT gate 0x80): a system call on the i386 ABI, which
             * Wine's WoW64 uses for set_thread_area; it enters the LibOS like `syscall` does */
            if (syscalls_emulate_int80(regs))
                break;
            /* Linux reports user-mode #GP as SIGSEGV with si_addr == NULL. Applications use
             * this behavior to probe instructions such as RDPMC and recover in a signal handler.
             * Forward the fault through the normal LibOS memory-fault path instead of treating
             * it as a fatal PAL exception. */
            ret = pal_common_perform_memfault_handling(/*faulted_addr=*/0, regs);
            if (ret < 0) {
                log_error("Panic: #GP handler failed (rip=0x%lx rsp=0x%lx error code=0x%lx)",
                          regs->rip, regs->rsp, regs->error_code);
                triple_fault();
            }
            break;
        case 14: ;
            /* below code is currently only for diagnostics; we always panic on PFs */
            uint64_t faulted_addr;
            __asm__ volatile("mov %%cr2, %%rax" : "=a"(faulted_addr));
#ifdef __x86_64__
            /* first touch of a demand-zero page of a process address space */
            if (memory_address_space_handle_fault(faulted_addr, regs->error_code))
                break;
#endif

            ret = pal_common_perform_memfault_handling(faulted_addr, regs);
            if (ret == 0) {
                /* LibOS successfully handled the memory fault */
                break;
            }

            faulted_addr &= ~0xFFFUL;

            uint64_t* pte_addr;
            ret = memory_find_page_table_entry(faulted_addr, &pte_addr);
            if (ret < 0) {
                log_error("Panic: #PF handler failed (cannot find PTE for 0x%lx)", faulted_addr);
                triple_fault();
            }

            log_error("Panic: #PF on address 0x%lx (corresponding PTE %p with value 0x%lx)",
                      faulted_addr, pte_addr, *pte_addr);
            log_error("       error code=0x%lx rip=0x%lx rsp=0x%lx rax=0x%lx", regs->error_code,
                      regs->rip, regs->rsp, regs->rax);
            triple_fault();
            break;
        case 20:
            ret = vm_virtualization_exception(regs);
            if (ret < 0) {
                log_error("Panic: virtualization exception (#VE, vector nr. 20) failed");
                triple_fault();
            }
            break;
        case 32: ;
            /* the other CPUs keep the 100 ms tick (their preemption quantum); CPU0 also serves the
             * timeouts, so its timer is armed for the earliest pending deadline, the tick being
             * the ceiling; a lapic_timer_kick_cpu0() IPI arrives here too */
            uint64_t timer_delay_us = LAPIC_TIMER_PERIOD_US;
            if (get_per_cpu_data()->cpu_id == 0) {
                /* only CPU0 handles timeouts and alarms */
                notify_about_timeouts_uninterruptable();
                timer_delay_us = timeouts_next_delay_us();
            }
            lapic_timer_rearm(timer_delay_us);
            if (regs->cs != kernel_cs) {
                /* only reschedule if timer interrupt occurs while in userland (i.e., we use
                 * preemptive userland scheduling but cooperative kernel scheduling); note that we
                 * don't enable/disable interrupts via RFLAGS' IF because it will happen
                 * automatically during save_context / restore_context */
#ifdef __x86_64__
                struct thread* curr_user = get_thread_ptr(rdmsr(MSR_IA32_GS_BASE));
                if (curr_user)
                    curr_user->preempt_pending = false;
                pal_common_deliver_pending_interrupt(regs);
#endif
                sched_thread_uninterruptable(regs);
            }
#ifdef __x86_64__
            else if (memory_frame_pool_active()) {
                /* kernel scheduling is cooperative, so a thread that spends its time in system
                 * calls is hardly ever preempted and starves the others (a parent polling
                 * waitpid(WNOHANG) for a child that cannot run); its next return to user mode
                 * takes this tick instead */
                struct thread* curr = get_thread_ptr(rdmsr(MSR_IA32_GS_BASE));
                if (curr && !curr->is_helper)
                    curr->preempt_pending = true;
            }
#endif
            break;
        case 33: ;
            /* "invalidate TLB" IPI -- may be spurious/extra, check for this */
            if (get_per_cpu_data()->invalidate_tlb_ipi_received) {
                /* ignore extra IPI (most probably there is a malicious host) */
                break;
            }
            bool in_progress = __atomic_load_n(&g_invalidate_tlb_request.in_progress,
                                               __ATOMIC_ACQUIRE);
            if (!in_progress) {
                /* ignore spurious IPI (most probably there is a malicious host) */
                break;
            }
            uint32_t old_num_responses = __atomic_load_n(&g_invalidate_tlb_request.num_responses,
                                                         __ATOMIC_ACQUIRE);
            if (!old_num_responses) {
                /* now this is weird -- some vCPU started the protocol but num of responses is 0 */
                log_error("Panic: `invalidate TLB` IPI received, but state is inconsistent");
                triple_fault();
            }

            void* addr  = __atomic_load_n(&g_invalidate_tlb_request.addr, __ATOMIC_ACQUIRE);
            size_t size = __atomic_load_n(&g_invalidate_tlb_request.size, __ATOMIC_ACQUIRE);
            if (size) {
                uint64_t mark_addr = (uint64_t)addr;
                while (mark_addr < (uint64_t)addr + size) {
                    invlpg(mark_addr);
                    mark_addr += PAGE_SIZE;
                }
            } else {
                /* A zero-size request invalidates a paging-structure change (e.g. a PML4 slot). */
                __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" : : : "rax", "memory");
            }

            get_per_cpu_data()->invalidate_tlb_ipi_received = 1;
            __atomic_fetch_add(&g_invalidate_tlb_request.num_responses, 1, __ATOMIC_ACQ_REL);
            lapic_signal_interrupt_complete();
            break;
        case 64:
            assert(get_per_cpu_data()->cpu_id == 0);
            ret = virtio_console_isr();
            if (ret < 0)
                triple_fault();
            ret = virtio_fs_isr();
            if (ret < 0)
                triple_fault();
            ret = virtio_vsock_isr();
            if (ret < 0)
                triple_fault();
            lapic_signal_interrupt_complete();
            break;
        default:
            log_error("Panic: unhandled exception (vector nr. %lu)", regs->int_number);
            log_error("       rip=0x%lx rsp=0x%lx rax=0x%lx", regs->rip, regs->rsp, regs->rax);
            triple_fault();
    }
}

int send_invalidate_tlb_ipi_and_wait(void* addr, size_t size, bool invalidate_on_this_cpu) {
    assert(IS_ALIGNED_PTR(addr, PAGE_SIZE) && IS_ALIGNED(size, PAGE_SIZE));

    int ret;

    if (invalidate_on_this_cpu && size) {
        uint64_t mark_addr = (uint64_t)addr;
        while (mark_addr < (uint64_t)addr + size) {
            invlpg(mark_addr);
            mark_addr += PAGE_SIZE;
        }
    } else if (invalidate_on_this_cpu) {
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" : : : "rax", "memory");
    }

    if (!g_interrupts_enabled) {
        /* this func may be called from bootstrap code, before interrupts are truly enabled */
        return 0;
    }

    spinlock_lock(&g_invalidate_tlb_request_lock);

    if (g_invalidate_tlb_request.in_progress || g_invalidate_tlb_request.num_responses) {
        /* sanity check that a previous "invalidate TLB" protocol run is finished */
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    for (uint32_t i = 0; i < g_num_cpus; i++) {
        /* reset the "IPI received" flag on each vCPU */
        g_per_cpu_data[i].invalidate_tlb_ipi_received = 0;
    }

    g_invalidate_tlb_request.in_progress = true;
    g_invalidate_tlb_request.addr = addr;
    g_invalidate_tlb_request.size = size;
    g_invalidate_tlb_request.num_responses = 1; /* this vCPU already invalidated */

    uint64_t icr_ipi_request = (/*destination=all_excluding_self*/3 << 18) + /*vector=*/33;
    vm_shared_wrmsr(MSR_INSECURE_IA32_LAPIC_ICR, icr_ipi_request);

    while (__atomic_load_n(&g_invalidate_tlb_request.num_responses, __ATOMIC_ACQUIRE)
            != g_num_cpus) {
        /* waiting for other vCPUs to invalidate their TLBs and acknowledge */
        CPU_RELAX();
    }

    g_invalidate_tlb_request.num_responses = 0;
    g_invalidate_tlb_request.in_progress = false;
    ret = 0;

out:
    spinlock_unlock(&g_invalidate_tlb_request_lock);
    return ret;
}

static int idt_gate_set(uint8_t isr_number, void* isr_addr) {
    /* selector, ist offset, flags, reserved bits are filled by *.S, check them here */
    if (g_idt[isr_number].code_selector == 0 ||
            g_idt[isr_number].ist_offset != 1 ||
            g_idt[isr_number].flags != 0x8E ||
            g_idt[isr_number]._reserved != 0) {
        return -PAL_ERROR_INVAL;
	}

    uint64_t isr_addr_uint64 = (uint64_t)isr_addr;
    g_idt[isr_number].isr_addr_low  = isr_addr_uint64 & 0xFFFF;
    g_idt[isr_number].isr_addr_mid  = (isr_addr_uint64 >> 16) & 0xFFFF;
    g_idt[isr_number].isr_addr_high = (isr_addr_uint64 >> 32) & 0xFFFFFFFF;
    return 0;
}

static int tss_init(void) {
    extern struct tss_64bitmode tss_64bitmode[1];
    extern struct tss_64bitmode_segment_descriptor tss_64bitmode_desc[1];

    /* *.S must have initialized TSS already (except for interrupt stack), check it */
    struct tss_64bitmode* tss = tss_64bitmode;
    tss += get_per_cpu_data()->cpu_id;

    if (tss->rsp0_unused != 0 || tss->rsp1_unused != 0 || tss->rsp2_unused != 0)
        return -PAL_ERROR_BADADDR;
    if (tss->ist1 != 0)
        return -PAL_ERROR_BADADDR;
    if (tss->iomap_base_unused != sizeof(*tss))
        return -PAL_ERROR_BADADDR;

    /* interrupt_stack is a base address, but we want top of the stack */
    tss->ist1 = (uint64_t)get_per_cpu_data()->interrupt_stack + INTERRUPT_STACK_SIZE;
#ifdef PAL_HOST_SEV
    if (sev_es_active())
        tss->ist2_unused = sev_vc_stack_top(get_per_cpu_data()->cpu_id);
#endif

    struct tss_64bitmode_segment_descriptor* tss_desc = tss_64bitmode_desc;
    tss_desc += get_per_cpu_data()->cpu_id;

    tss_desc->base_low_16bits  = (uint64_t)tss & 0xFFFF;
    tss_desc->base_low_8bits   = ((uint64_t)tss >> 16) & 0xFF;
    tss_desc->base_mid_8bits   = ((uint64_t)tss >> 24) & 0xFF;
    tss_desc->base_high_32bits = ((uint64_t)tss >> 32) & 0xFFFFFFFF;
    tss_desc->limit_low_16bits = sizeof(*tss) & 0xFFFF;
    return 0;
}

static int idt_init(void) {
    extern struct idt_gate idt_start[256]; /* IDT from *.S */
    g_idt = (struct idt_gate*)&idt_start;

    int ret;

    if (get_per_cpu_data()->cpu_id != 0) {
        /* all CPUs share the same IDT, so enough to initialize IDT only once */
        return 0;
    }

    /* hardware interrupts */
    ret = idt_gate_set(0,  &isr_0);  /* Divide-by-zero Error (#DE) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(1,  &isr_1);  /* Debug (#DB) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(2,  &isr_2);  /* Non-maskable Interrupt (NMI) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(3,  &isr_3);  /* Breakpoint (#BP) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(4,  &isr_4);  /* Overflow (#OF) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(5,  &isr_5);  /* Bound Range Exceeded (#BR) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(6,  &isr_6);  /* Invalid Opcode (#UD) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(7,  &isr_7);  /* Device Not Available (#NM) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(8,  &isr_8);  /* Double Fault (#DF) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(9,  &isr_9);  /* <legacy> */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(10, &isr_10); /* Invalid TSS (#TS) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(11, &isr_11); /* Segment Not Present (#NP) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(12, &isr_12); /* Stack-Segment Fault (#SS) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(13, &isr_13); /* General Protection Fault (#GP) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(14, &isr_14); /* Page Fault (#PF) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(15, &isr_15); /* <reserved> */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(16, &isr_16); /* x87 FP Exception (#MF) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(17, &isr_17); /* Alignment Check (#AC) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(18, &isr_18); /* Machine Check (#MC) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(19, &isr_19); /* SIMD FP Exception (#XM) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(20, &isr_20); /* Virtualization Exception (#VE) */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;
#ifdef PAL_HOST_SEV
    if (sev_es_active()) {
        /* VMM Communication exception (#VC, SEV-ES/SNP) on its own stack (IST2): it is raised
         * inside other interrupt handlers too (EOI, timer MSRs), see pal/src/host/vm/sev/ */
        ret = idt_gate_set(29, &isr_vc);
        if (ret < 0)
            return -PAL_ERROR_BADADDR;
        g_idt[29].ist_offset = 2;
    }
#endif

    ret = idt_gate_set(32, &isr_32); /* Local APIC timer IRQ */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(33, &isr_33); /* "Invalidate TLB" IPI interrupt */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(39, &isr_spurious);
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    ret = idt_gate_set(64, &isr_64); /* generic virtio devices IRQ */
    if (ret < 0)
        return -PAL_ERROR_BADADDR;

    return 0;
}

/*
 * One GDT per CPU: Linux's layout for the first 16 entries (the null, kernel and user
 * descriptors of kernel_static_structs.S in 0 to 6, unused 7 to 11, the TLS slots of
 * set_thread_area in 12 to 14) and the CPU's TSS descriptor at entry 16. The static GDT of
 * kernel_static_structs.S stays the boot GDT and the template, so every selector computed from
 * its labels (STAR, the IDT's kernel CS, the user selectors) keeps its value. The TLS slots are
 * per thread: the switch-in (kernel_sched.c) writes the incoming thread's descriptors here before
 * it reloads %fs, and set_thread_area writes the running thread's.
 */
#define GDT_ENTRIES_BEFORE_TSS 16
struct gdt_per_cpu {
    uint64_t entries[GDT_ENTRIES_BEFORE_TSS];
    struct tss_64bitmode_segment_descriptor tss;
} __attribute__((aligned(64)));
static_assert(GDT_ENTRY_TLS_MIN + GDT_ENTRIES_TLS <= GDT_ENTRIES_BEFORE_TSS, "TLS slots");
static struct gdt_per_cpu g_gdt_per_cpu[MAX_NUM_CPUS];

void gdt_load_tls(const uint64_t tls_desc[GDT_ENTRIES_TLS]) {
    struct gdt_per_cpu* gdt = &g_gdt_per_cpu[get_per_cpu_data()->cpu_id];
    memcpy(&gdt->entries[GDT_ENTRY_TLS_MIN], tls_desc, GDT_ENTRIES_TLS * sizeof(uint64_t));
}

int interrupts_init(void) {
    extern char gdt_start[1];
    extern struct tss_64bitmode_segment_descriptor tss_64bitmode_desc[1];

    int ret;

    ret = tss_init();
    if (ret < 0)
        return ret;

    ret = idt_init();
    if (ret < 0)
        return ret;

    /* this CPU's GDT: the template's descriptors (which end where its TSS descriptors start),
     * empty TLS slots and this CPU's TSS descriptor as tss_init() filled it in the template */
    uint32_t cpu_id = get_per_cpu_data()->cpu_id;
    struct gdt_per_cpu* gdt = &g_gdt_per_cpu[cpu_id];
    size_t template_size = (char*)tss_64bitmode_desc - gdt_start;
    if (template_size > GDT_ENTRY_TLS_MIN * sizeof(uint64_t))
        return -PAL_ERROR_BADADDR;
    memset(gdt, 0, sizeof(*gdt));
    memcpy(gdt->entries, gdt_start, template_size);
    gdt->tss = tss_64bitmode_desc[cpu_id];

    /* load our GDT and IDT */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr_value = { .limit = sizeof(*gdt) - 1, .base = (uint64_t)gdt };
    extern char idtr[1];
    __asm__ volatile("lgdt %0" :: "m"(gdtr_value));
    __asm__ volatile("lidt %0" :: "m"(idtr));

    /* flush TSS descriptor */
    ltr(offsetof(struct gdt_per_cpu, tss));

#ifdef PAL_HOST_VM
    g_idt_loaded = true;
#endif
    /* enable interrupts */
    sti();
    return 0;
}
