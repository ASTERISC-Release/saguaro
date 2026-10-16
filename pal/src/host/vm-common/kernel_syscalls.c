/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * System calls via SYSCALL/SYSRET in 64-bit mode. See also the *.S code.
 *
 * Notes on multi-core synchronization:
 *   - all syscall operations happen only on init, no sync required
 */

#include <stdint.h>

#include "api.h"

#include "kernel_interrupts.h"
#include "kernel_multicore.h"
#include "kernel_syscalls.h"
#include "pal_common.h"

extern void syscall_asm(void);
extern void syscall_asm_gs(void);
extern void int80_ret_asm(void);

#define AMD_CPU_FIX

int syscalls_init(void) {
    extern char gdt_start[1];
    extern char gdt_entry_kernel_cs[1];
    extern char gdt_entry_user_cs[1];

    /*
     * IA32_STAR MSR has weird semantics (for 64-bit mode with SYSCALL/SYSRET):
     *   - bits 32-47 must contain a kernel code selector offset, and kernel data selector is
     *     computed as +8 to this kernel-code-selector value (so kernel data segment descriptor
     *     must immediatelly follow the kernel code segment descriptor in our bootloader)
     *   - bits 48-63 must contain a "user code selector offset minus 16", and user data selector
     *     is computed as "user code selector offset minus 8" (so user data segment descriptor must
     *     immediatelly preceed the user code segment descriptor)
     *
     * Note that the code selector is 64-bit-mode but the corresponding data selector is 32-bit-mode
     * in our GDT entries. However, we rely on the weird fact that syscall/sysret instructions do
     * not really load selectors from GDT but instead load fixed values (including switching to
     * 64-bit-mode), so our layout of GDT entries still works.
     */

#ifdef AMD_CPU_FIX
    /* STAR wants user CS selector minus 16, with RPL=3 set (sysretq will load CS=+16, SS=+8) */
    uint64_t user_cs_sel = (uint64_t)(gdt_entry_user_cs - gdt_start) | 0x3;
    wrmsr(MSR_STAR,
        (user_cs_sel << 48) | ((gdt_entry_kernel_cs - gdt_start) << 32));
#else 
    wrmsr(MSR_STAR,
        ((gdt_entry_user_cs - gdt_start) << 48) | ((gdt_entry_kernel_cs - gdt_start) << 32));
#endif 

    /* IA32_LSTAR MSR simply contains the 64-bit address of the syscall entry */
    wrmsr(MSR_LSTAR, (uint64_t)&syscall_asm);
    get_per_cpu_data()->lstar_gs = 0;

    /* IA32_SYSCALL_MASK MSR contains RFLAGS that are cleared on `syscall` (taken from Linux);
     * we don't clear the Interrupt Flag because our kernel is ok to be interrupted */
    wrmsr(MSR_SYSCALL_MASK, (uint64_t)(/*trap flag*/          1 << 8  |
                                       /*direction flag*/     1 << 10 |
                                       /*IO privilege level*/ 3 << 12 |
                                       /*nested task*/        1 << 14 |
                                       /*alignment check*/    1 << 18));

    return 0;
}

/*
 * `int $0x80` is the i386 system-call ABI, which Linux also serves from 64-bit code: the number
 * in %eax, the arguments in ebx, ecx, edx, esi, edi, ebp, only %eax changed on return. Wine's
 * new WoW64 uses it for set_thread_area (and modify_ldt), see design-32bit.md. Without an IDT
 * gate the instruction raises #GP in user mode with error code 0x402 (bit 1: IDT; vector 0x80;
 * not external). The frame is turned into the entry `syscall_asm` would have made: the LibOS is
 * entered in ring 0, with the x86_64 number and argument registers, the user RIP (after the two
 * instruction bytes) in the TCB, RCX = the return stub int80_ret_asm and R11 = the user RFLAGS,
 * GS kept at the PAL TCB (the ISR entry swapped it in; the stub loads the thread's own base
 * back). The stub returns with iretq and the user's own registers (kept in the thread), as
 * Linux preserves every register but eax across `int $0x80`. Only the numbers below are served;
 * the others return -ENOSYS at once.
 */
bool syscalls_emulate_int80(struct isr_regs* regs) {
    static const struct {
        uint32_t i386_nr;
        uint32_t x86_64_nr;
    } table[] = {
        { 243, 205 }, /* set_thread_area */
        { 244, 211 }, /* get_thread_area */
        { 123, 154 }, /* modify_ldt */
    };

    if (regs->error_code != ((0x80 << 3) | 2) || (regs->cs & 3) != 3)
        return false;

    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    if (!tcb || !tcb->common.libos_tcb)
        return false;

    regs->rip += 2; /* `int $0x80` is cd 80 */

    uint32_t x86_64_nr = 0;
    bool found = false;
    for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
        if (table[i].i386_nr == (uint32_t)regs->rax) {
            x86_64_nr = table[i].x86_64_nr;
            found = true;
            break;
        }
    }
    if (!found) {
        regs->rax = (uint64_t)-38; /* -ENOSYS */
        return true;
    }

    /* the user's registers, which int80_ret_asm restores on the return (the LibOS restores the
     * registers of the entry, the i386 arguments below, and `syscall`'s return clobbers RCX and
     * R11) */
    struct thread* thread = &tcb->kernel_thread;
    thread->int80_regs[0] = regs->rdi;
    thread->int80_regs[1] = regs->rsi;
    thread->int80_regs[2] = regs->rdx;
    thread->int80_regs[3] = regs->r10;
    thread->int80_regs[4] = regs->r8;
    thread->int80_regs[5] = regs->r9;
    thread->int80_regs[6] = regs->rcx;
    thread->int80_regs[7] = regs->r11;
    thread->int80_rflags  = regs->rflags;

    uint64_t ebx = (uint32_t)regs->rbx, ecx = (uint32_t)regs->rcx, edx = (uint32_t)regs->rdx;
    uint64_t esi = (uint32_t)regs->rsi, edi = (uint32_t)regs->rdi, ebp = (uint32_t)regs->rbp;
    regs->rdi = ebx;
    regs->rsi = ecx;
    regs->rdx = edx;
    regs->r10 = esi;
    regs->r8  = edi;
    regs->r9  = ebp;
    regs->rax = x86_64_nr;

    thread->context.user_rip = regs->rip;
    regs->r11 = regs->rflags;
    regs->rcx = (uint64_t)&int80_ret_asm;
    /* `libos_syscall_entry` is at offset 8 of the LibOS TCB (as syscall_asm jumps to it) */
    regs->rip = *(uint64_t*)((char*)tcb->common.libos_tcb + 8);

    extern char gdt_start[1];
    extern char gdt_entry_kernel_cs[1];
    regs->cs = (uint64_t)(gdt_entry_kernel_cs - gdt_start);
    regs->ss = regs->cs + 8; /* the kernel data descriptor follows the code one */
    /* the flags `syscall` clears on entry (MSR_SYSCALL_MASK); R11 keeps the user's */
    regs->rflags &= ~(uint64_t)(1 << 8 | 1 << 10 | 3 << 12 | 1 << 14 | 1 << 18);
    regs->gs_swapped = 0;
    return true;
}

void syscalls_select_entry(struct per_cpu_data* cpu, bool gs) {
    if (cpu->lstar_gs == (uint8_t)gs)
        return;
    wrmsr(MSR_LSTAR, gs ? (uint64_t)&syscall_asm_gs : (uint64_t)&syscall_asm);
    cpu->lstar_gs = gs;
}
