/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

#include "asm-offsets.h"

.macro globl sym
.globl \sym
.hidden \sym
.endm

    globl   isr_spurious
    globl   isr_iret_to_userland
    globl   jump_into_user_mode
    globl   syscall_asm
    globl   syscall_asm_gs
    globl   vm_jump_to_user_gs
    globl   save_context_and_restore_next
    globl   do_xsave
    globl   do_xrstor
    globl   thread_main_wrapper

.macro  isrstub num, has_errorcode
    globl   isr_\num
isr_\num:
    .ifb    \has_errorcode
    push    $0
    .endif
    push    $\num
    jmp     _isr_common
.endm

    isrstub 0
    isrstub 1
    isrstub 2
    isrstub 3
    isrstub 4
    isrstub 5
    isrstub 6
    isrstub 7
    isrstub 8, 1
    isrstub 9
    isrstub 10, 1
    isrstub 11, 1
    isrstub 12, 1
    isrstub 13, 1
    isrstub 14, 1
    isrstub 15
    isrstub 16
    isrstub 17, 1
    isrstub 18
    isrstub 19
    isrstub 20
    isrstub 32   // Local APIC timer interrupt (in TSC-deadline mode)
    isrstub 33   // "Invalidate TLB" IPI interrupt (used when updating page table entries)
    isrstub 64   // virtio devices interrupt (console, fs, vsock)

isr_spurious:
    iretq

isr_iret_to_userland:
    cmpq    $0, %gs:PAL_TCB_VM_GS_SWAPPED_BY_ISR
    je      1f
    // the resumed context ran with the thread's own GS base: load it; a register's home is the
    // per-CPU scratch slot, reached through swapgs (KERNEL_GS_BASE stays the per-CPU data after
    // each pair)
    cli
    movq    $0, %gs:PAL_TCB_VM_GS_SWAPPED_BY_ISR
    swapgs
    mov     %rax, %gs:PER_CPU_DATA_GS_SCRATCH
    swapgs
    mov     %gs:PAL_TCB_VM_USER_GSBASE, %rax
    wrgsbase %rax
    swapgs
    mov     %gs:PER_CPU_DATA_GS_SCRATCH, %rax
    swapgs
1:
    iretq

_isr_common:
    push    %rcx
    push    %rax
    push    %rdx
    push    %rbx
    push    %rbp
    push    %rsi
    push    %rdi
    push    %r15
    push    %r14
    push    %r13
    push    %r12
    push    %r11
    push    %r10
    push    %r9
    push    %r8

    mov     $MSR_IA32_GS_KERNEL_BASE, %rcx
    rdmsr
    shl     $32, %rdx
    or      %rdx, %rax

    // A context that owns %gs was interrupted with its own GS base (user mode, or a signal handler
    // that the LibOS runs in ring 0 with the application's GS): put the running thread's TCB
    // (per-CPU current_tcb) into GS before any %gs use and remember it for the return. The rule
    // is "GS equals the thread's recorded base": a dummy TCB in GS (thread exit) or another TCB
    // (a switch in progress) is left alone. Threads without their own %gs skip this at the first
    // compare.
    xor     %r8d, %r8d                      // isr_regs.gs_swapped (r8 was saved above)
    mov     PER_CPU_DATA_CURRENT_TCB(%rax), %rdi
    test    %rdi, %rdi
    jz      .Lisr_gs_ok
    mov     PAL_TCB_VM_USER_GSBASE(%rdi), %rsi
    test    %rsi, %rsi
    jz      .Lisr_gs_ok
    rdgsbase %rdx
    cmp     %rdx, %rsi
    jne     .Lisr_gs_ok
    wrgsbase %rdi
    mov     $1, %r8d
.Lisr_gs_ok:
    push    %r8                             // isr_regs.gs_swapped

    mov     PER_CPU_DATA_XSAVE_AREA(%rax), %rdi
    push    %rdi
    callq   do_xsave

    mov     %rsp, %rdi

    // preserve stack ptr in RBX (callee-saved) and 16B-align it (required by System V AMD64 ABI)
    mov     %rsp, %rbx
    and     $~0xF, %rsp
    call    isr_c
    mov     %rbx, %rsp

    pop     %rdi
    callq   do_xrstor

    // returning to the context the entry swapped the TCB in over (isr_regs.gs_swapped): its base
    // goes back in (with interrupts off until iretq restores RFLAGS; the registers below are
    // popped afterwards)
    pop     %rdi
    test    %rdi, %rdi
    jz      .Lisr_ret_gs_done
    cli
    mov     %gs:PAL_TCB_VM_USER_GSBASE, %rdi
    wrgsbase %rdi
.Lisr_ret_gs_done:

    pop     %r8
    pop     %r9
    pop     %r10
    pop     %r11
    pop     %r12
    pop     %r13
    pop     %r14
    pop     %r15
    pop     %rdi
    pop     %rsi
    pop     %rbp
    pop     %rbx
    pop     %rdx
    pop     %rax
    pop     %rcx
    add     $16, %rsp   // jump over int_number and error_code on the stack
    iretq


jump_into_user_mode:
    pushfq
    pop     %r11                // sysretq expects user-land RFLAGS in R11, move it via pushf/pop
    mov     %rdi, %rcx          // sysretq expects user-land RIP (first arg) in RCX
    xor     %r15, %r15
    xor     %r14, %r14
    xor     %r13, %r13
    xor     %r12, %r12
    xor     %r10, %r10
    xor     %r9, %r9
    xor     %r8, %r8
    xor     %rbp, %rbp
    xor     %rbx, %rbx
    xor     %rax, %rax
    xor     %rdx, %rdx
    xor     %rsi, %rsi
    xor     %rdi, %rdi
    and     $~0xF, %rsp         // align stack by 16B
    sysretq


// The syscall entry of a thread that owns %gs (arch_prctl(ARCH_SET_GS)): LSTAR points here while
// such a thread runs (sched_switch_to_current_thread_root), to syscall_asm otherwise, so threads
// without their own %gs execute syscall_asm unchanged. On entry GS is the application's; the
// running thread's TCB comes from the per-CPU data (through swapgs, which leaves KERNEL_GS_BASE
// as the per-CPU data after each pair), RAX lives in the per-CPU scratch slot meanwhile, the
// application's base is remembered in the TCB and the TCB goes into GS. Interrupts are off for
// these few instructions (an interrupt would read KERNEL_GS_BASE for the per-CPU data).
syscall_asm_gs:
    cli
    swapgs
    mov     %rax, %gs:PER_CPU_DATA_GS_SCRATCH
    mov     %gs:PER_CPU_DATA_CURRENT_TCB, %rax
    swapgs
    mov     %rcx, PAL_TCB_VM_USER_RIP(%rax)
    rdgsbase %rcx
    cmp     %rcx, %rax
    je      1f              // GS already holds the TCB (a syscall from code the LibOS entered
                            // directly, such as a signal handler): keep the remembered base
    mov     %rcx, PAL_TCB_VM_USER_GSBASE(%rax)
    wrgsbase %rax
1:
    swapgs
    mov     %gs:PER_CPU_DATA_GS_SCRATCH, %rax
    swapgs
    sti
    cmp     $SYSCALL_RT_SIGRETURN, %rax
    je      .Linstall_rt_sigreturn_path
    jmp     .Linstall_sysret_path

// The LibOS jumps to a signal handler of the application directly (libos_context.c, the VM
// branch; the handler then runs in ring 0, as always on this PAL). For a thread that owns %gs
// the LibOS puts the handler's address into the TCB (user_rip) and jumps here instead: the
// application's base goes into GS and the handler is entered with every register as the LibOS
// left it (RAX travels through the per-CPU scratch slot). Interrupts stay off until the jump;
// the handler's context has IF set (the LibOS gave it RFLAGS 0x202; this stub is reached with
// those flags, and sti restores them).
vm_jump_to_user_gs:
    cli
    push    %rax
    mov     %gs:PAL_TCB_VM_USER_RIP, %rax
    xchg    %rax, (%rsp)                    // the target's address below the stack, RAX back
    push    %rax
    mov     %gs:PAL_TCB_VM_USER_GSBASE, %rax
    wrgsbase %rax
    pop     %rax
    sti
    ret

// FIXME: ideally must add swapgs in the very beginning, but GS is not used by ring-3 anyway
syscall_asm:
    mov     %rcx, %gs:PAL_TCB_VM_USER_RIP
    cmp     $SYSCALL_RT_SIGRETURN, %rax
    je      .Linstall_rt_sigreturn_path
.Linstall_sysret_path:
    lea     sysret_asm(%rip), %rcx
    jmp     .Lsyscall_libos
.Linstall_rt_sigreturn_path:
    lea     sigreturn_asm(%rip), %rcx
.Lsyscall_libos:
    jmp     *%gs:(PAL_TCB_LIBOS + 0x8)      // `libos_syscall_entry` addr is at offset 0x8

// FIXME: ideally must add swapgs right-before sysretq, but GS is not used by ring-3 anyway
sysret_asm:
    cmpb    $0, %gs:PAL_TCB_VM_PREEMPT_PENDING
    jne     .Lsysret_preempt
.Lsysret_now:
    mov     %gs:PAL_TCB_VM_USER_RIP, %rcx
    or      $0x200, %r11                    // userland RFLAGS will contain IF
    cmpq    $0, %gs:PAL_TCB_VM_USER_GSBASE
    jne     .Lsysret_gs
    sysretq

    // the thread owns %gs: its base goes back in (RAX, the return value, waits in the per-CPU
    // scratch slot; sysretq restores IF from R11)
.Lsysret_gs:
    cli
    swapgs
    mov     %rax, %gs:PER_CPU_DATA_GS_SCRATCH
    swapgs
    mov     %gs:PAL_TCB_VM_USER_GSBASE, %rax
    wrgsbase %rax
    swapgs
    mov     %gs:PER_CPU_DATA_GS_SCRATCH, %rax
    swapgs
    sysretq

    // a timer tick found this thread in kernel mode: with interrupts off, make the timer fire
    // now; it is taken right after sysretq, in user mode, where it preempts the thread
.Lsysret_preempt:
    cli
    movb    $0, %gs:PAL_TCB_VM_PREEMPT_PENDING
    push    %rax
    push    %rdx
    push    %rsi
    push    %rdi
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %rbp
    mov     %rsp, %rbp
    and     $~0xF, %rsp
    call    lapic_timer_fire_now
    mov     %rbp, %rsp
    pop     %rbp
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rdi
    pop     %rsi
    pop     %rdx
    pop     %rax
    jmp     .Lsysret_now

// FIXME: ideally must add swapgs right-before sysretq, but GS is not used by ring-3 anyway
sigreturn_asm:
    // memorize user RSP because need to switch to pseudo stack for iretq
    mov     %rsp, %gs:PAL_TCB_VM_SIGRETURN_USER_RSP
    // memorize user RAX because need one temporary register to push values to pseudo stack
    mov     %rax, %gs:PAL_TCB_VM_SIGRETURN_USER_RAX

    mov     %gs:PAL_TCB_VM_SIGRETURN_PSEUDO_RSP, %rsp
    // push SS: 64-bit user data
    pushq   $0x23 // SS segment selector: Index=100 (4), GDT, RPL=11
    // push user RSP
    mov     %gs:PAL_TCB_VM_SIGRETURN_USER_RSP, %rax
    push    %rax
    // push user RFLAGS; make sure it contains IF
    pushfq
    pop     %rax
    or      $0x200, %rax
    push    %rax
    // push CS: 64-bit user code
    pushq   $0x2b // CS segment selector: Index=101 (5), GDT, RPL=11
    // push user RIP
    mov     %gs:PAL_TCB_VM_USER_RIP, %rax
    push    %rax
    // RSP will be restored by iretq, but RAX we must restore manually
    mov     %gs:PAL_TCB_VM_SIGRETURN_USER_RAX, %rax
    cmpq    $0, %gs:PAL_TCB_VM_USER_GSBASE
    je      .Lsigreturn_iret
    cli
    swapgs
    mov     %rax, %gs:PER_CPU_DATA_GS_SCRATCH
    swapgs
    mov     %gs:PAL_TCB_VM_USER_GSBASE, %rax
    wrgsbase %rax
    swapgs
    mov     %gs:PER_CPU_DATA_GS_SCRATCH, %rax
    swapgs
.Lsigreturn_iret:
    // jump to user context
    iretq


    // void save_context_and_restore_next(uint64_t curr_gs_base, uint64_t next_gs_base,
    //                                    uint32_t* lock_to_unlock, int* clear_child_tid,
    //                                    uint32_t* critical_section_lock_to_unlock,
    //                                    void* scheduling_stack,
    //                                    void (*post_switch)(void*), void* post_switch_arg)
    //
    //   RDI (1st arg):  current context (= GS register base), may be 0x0
    //   RSI (2nd arg):  next context (= next GS register base), always exists
    //   RDX (3rd arg):  lock that was taken and needs to be released
    //   RCX (4th arg):  memory that should be zeroed (signal to parent that child exited)
    //   R8  (5th arg):  lock of the critical section that ends here, needs to be released
    //   R9  (6th arg):  temporary stack on which to put this func's local variables and pop rflags
save_context_and_restore_next:
    // note that r10 and r11 are caller-saved regs, so we can overwrite them freely

    // Store the original RFLAGS of the context as the very first operation, because later
    // operations (e.g. `test` instruction below) modify RFLAGS; this may be not needed if there is
    // no current context, but in that case we'll only clobber RAX which is benign.
    // We are possibly modifying the prev-thread stack, but we are under the
    // critical_section_lock_to_unlock() lock so this stack can't be raced on by another CPU.
    pushf
    pop     %rax

    test    %rdi, %rdi             // no curr_gs_base -- no context to save
    jz      .Lrestore_next_context

.Lsave_current_context:
    mov     %r8,  %gs:PAL_TCB_VM_CONTEXT_R8
    mov     %r9,  %gs:PAL_TCB_VM_CONTEXT_R9
    mov     %r10, %gs:PAL_TCB_VM_CONTEXT_R10
    mov     %r11, %gs:PAL_TCB_VM_CONTEXT_R11
    mov     %r12, %gs:PAL_TCB_VM_CONTEXT_R12
    mov     %r13, %gs:PAL_TCB_VM_CONTEXT_R13
    mov     %r14, %gs:PAL_TCB_VM_CONTEXT_R14
    mov     %r15, %gs:PAL_TCB_VM_CONTEXT_R15
    mov     %rdi, %gs:PAL_TCB_VM_CONTEXT_RDI
    mov     %rsi, %gs:PAL_TCB_VM_CONTEXT_RSI
    mov     %rbp, %gs:PAL_TCB_VM_CONTEXT_RBP
    mov     %rbx, %gs:PAL_TCB_VM_CONTEXT_RBX
    mov     %rdx, %gs:PAL_TCB_VM_CONTEXT_RDX
    movq    $0,   %gs:PAL_TCB_VM_CONTEXT_RAX
    mov     %rcx, %gs:PAL_TCB_VM_CONTEXT_RCX
    mov     %rsp, %gs:PAL_TCB_VM_CONTEXT_RSP

    // RAX contains current-context original RFLAGS; mark interrupts as enabled in the restored
    // context, this is because current context may be executed under `spinlock_lock_disable_irq()`
    or      $(1 << 9), %rax
    mov     %rax, %gs:PAL_TCB_VM_CONTEXT_RFLAGS

    lea     .Lrestore_saved_context(%rip), %rax
    mov     %rax, %gs:PAL_TCB_VM_CONTEXT_RIP

    // RDX (function arg `lock_to_unlock`) may be clobbered by do_xsave, so save on stack
    push    %rdx
    mov     %gs:PAL_TCB_VM_CONTEXT_FPREGS, %rdi
    callq   do_xsave
    pop     %rdx

.Lrestore_next_context:
    push    %rdx
    push    %rcx
    mov     %rsi, %rax              // low-order 32 bits of `next_gs_base`
    mov     %rsi, %rdx              // high-order 32 bits of `next_gs_base` (shifted-right below)
    shr     $32, %rdx
    mov     $MSR_IA32_GS_BASE, %rcx
    wrmsr
    pop     %rcx
    pop     %rdx

    // R12 was saved in the old context above. Always replace it: when lock_to_unlock is NULL,
    // retaining the old R12 would make the deferred unlock write through an arbitrary pointer.
    mov     %rdx, %r12              // keep lock until on the scheduling stack

    mov     8(%rsp), %r10           // optional post-switch cleanup function
    mov     16(%rsp), %r11          // optional post-switch cleanup argument

    cmp     $0, %rcx                // check if clear_child_tid != NULL
    je      .Lskip_clear_child_tid
    movl    $0, (%rcx)              // set *clear_child_tid = 0
.Lskip_clear_child_tid:

    // Before we leave the critical section, need to shift to a temporary "scheduling" stack,
    // otherwise the operations below could modify the prev-thread stack and have a data race with
    // some CPU that simultaneously scheduled this thread and also modifies the stack.
    // The above is irrelevant if we currently execute in the interrupt handling context because
    // then we're operating on the interrupt stack which is per-CPU and cannot be preempted, thus no
    // data race on the interrupt stack is possible. (In this case, scheduling_stack = NULL.)
    cmp     $0, %r9                 // check if scheduling_stack != NULL
    je      .Lskip_setting_scheduling_stack
    mov     %r9, %rsp             // set this temporary scheduling stack
.Lskip_setting_scheduling_stack:

    // Install the next thread's page-table root only now: the previous thread's stack (for
    // example a LibOS syscall stack in process-private memory) may be mapped differently, or not
    // at all, in the next root, while the scheduling stack and the interrupt stack are PAL memory
    // mapped identically in every root. GS already holds the next thread's TCB.
    push    %r8
    push    %r10
    push    %r11
    push    %r12
    callq   sched_switch_to_current_thread_root
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r8

    // Now we are completely done with both previously running thread and next thread, can unlock
    // the critical section (typically `g_thread_list_lock`). Note that interrupts are re-enabled
    // (if that was the state of the restoring thread) during popfq below.
    movl    $0, (%r8)              // spinlock_unlock(critical_section_lock_to_unlock)

    // Cleanup may destroy the old root and its stacks; run only after switching roots and stacks
    // and after releasing scheduler/stack locks.
    test    %r12, %r12
    jz      .Lskip_deferred_unlock
    movl    $0, (%r12)
.Lskip_deferred_unlock:
    test    %r10, %r10
    jz      .Lskip_post_switch
    mov     %r11, %rdi
    call    *%r10
.Lskip_post_switch:

    mov     %gs:PAL_TCB_VM_CONTEXT_FPREGS, %rdi
    callq   do_xrstor

    mov     %gs:PAL_TCB_VM_CONTEXT_RFLAGS, %rax
    push    %rax
    popfq

    mov     %gs:PAL_TCB_VM_CONTEXT_R8,  %r8
    mov     %gs:PAL_TCB_VM_CONTEXT_R9,  %r9
    mov     %gs:PAL_TCB_VM_CONTEXT_R10, %r10
    mov     %gs:PAL_TCB_VM_CONTEXT_R11, %r11
    mov     %gs:PAL_TCB_VM_CONTEXT_R12, %r12
    mov     %gs:PAL_TCB_VM_CONTEXT_R13, %r13
    mov     %gs:PAL_TCB_VM_CONTEXT_R14, %r14
    mov     %gs:PAL_TCB_VM_CONTEXT_R15, %r15
    mov     %gs:PAL_TCB_VM_CONTEXT_RDI, %rdi
    mov     %gs:PAL_TCB_VM_CONTEXT_RSI, %rsi
    mov     %gs:PAL_TCB_VM_CONTEXT_RBP, %rbp
    mov     %gs:PAL_TCB_VM_CONTEXT_RBX, %rbx
    mov     %gs:PAL_TCB_VM_CONTEXT_RDX, %rdx
    mov     %gs:PAL_TCB_VM_CONTEXT_RCX, %rcx
    mov     %gs:PAL_TCB_VM_CONTEXT_RSP, %rsp
    mov     %gs:PAL_TCB_VM_CONTEXT_RAX, %rax

    jmp     *%gs:PAL_TCB_VM_CONTEXT_RIP

.Lrestore_saved_context:
    ret


    // void __do_xsave(PAL_XREGS_STATE* xsave_area)
    //   RDI (argument):        pointer to xsave_area
    //   R11 (return address):  in order to not touch stack
    //   RAX, RDX:              clobbered
__do_xsave:
    movq    $0, XSAVE_HEADER_OFFSET + 0 * 8(%rdi)    // clear xsave header
    movq    $0, XSAVE_HEADER_OFFSET + 1 * 8(%rdi)
    movq    $0, XSAVE_HEADER_OFFSET + 2 * 8(%rdi)
    movq    $0, XSAVE_HEADER_OFFSET + 3 * 8(%rdi)
    movq    $0, XSAVE_HEADER_OFFSET + 4 * 8(%rdi)
    movq    $0, XSAVE_HEADER_OFFSET + 5 * 8(%rdi)
    movq    $0, XSAVE_HEADER_OFFSET + 6 * 8(%rdi)
    movq    $0, XSAVE_HEADER_OFFSET + 7 * 8(%rdi)

    movl    $0xffffffff, %eax
    movl    $0xffffffff, %edx
    xsave64 (%rdi)
    jmp     *%r11

    // void do_xsave(PAL_XREGS_STATE* xsave_area)
do_xsave:
    popq    %r11
    jmp     __do_xsave

    // void __do_xrstor(const PAL_XREGS_STATE* xsave_area)
    //   RDI (argument):        pointer to xsave_area
    //   R11 (return address):  in order to not touch stack
    //   RAX, RDX:              clobbered
__do_xrstor:
    movl     $0xffffffff, %eax
    movl     $0xffffffff, %edx
    xrstor64 (%rdi)
    jmp      *%r11

    // void do_xrstor(const PAL_XREGS_STATE* xsave_area)
do_xrstor:
    popq %r11
    jmp __do_xrstor


    // void thread_main_wrapper(void* callback_args, void* callback, void* terminate_func)
    //
    //   RDI (1st arg):  callback args  -- arguments of the main function of the starting thread
    //   RSI (2nd arg):  callback       -- main function of the starting thread
    //   RDX (3rd arg):  terminate_func -- function to terminate the thread
    //
    // This function is required to support callback functions that perform `return 0` instead of
    // `PalThreadExit()` at the end of thread execution. Without this wrapper, the thread would jump
    // off the stack upon `return 0`.
    //
thread_main_wrapper:
    push  %rdx
    callq *%rsi

    pop   %rdx
    xor   %rdi, %rdi
    callq *%rdx
    ud2
