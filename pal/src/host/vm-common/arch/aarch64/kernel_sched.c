/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_sched.c.
 *
 * The round-robin SQMS scheduling logic is architecture-neutral and copied as-is. What changes
 * is the register plumbing around it:
 *   - "current thread TCB" comes from the per-CPU slot `cur_tcb` (x86: rdmsr(GS_BASE)), because
 *     aarch64 has no kernel-only TLS register; see arch/aarch64/kernel_multicore.h.
 *   - "current user TLS" is `mrs/msr tpidr_el0` (x86: rdmsr/wrmsr(FS_BASE)) -- reading/writing
 *     the *current thread's* EL0 TLS register is architecturally allowed from EL1.
 *   - preempting a thread that ran in EL0 saves its exception frame (ELR_EL1/SPSR_EL1/SP_EL0 +
 *     GPRs, handed over by the el0_irq asm handler as `struct isr_regs`) and rewires `pc` to
 *     the asm trampoline that re-enters EL0 via `eret` (x86: `isr_iret_to_userland` + iretq).
 */

#include "kernel_sched.h"

#include <stdint.h>

#include "api.h"
#include "cpu.h"
#include "kernel_interrupts.h"
#include "kernel_multicore.h"
#include "kernel_thread.h"
#include "kernel_virtio.h"
#include "kernel_xsave.h"
#include "pal.h"
#include "pal_error.h"

/* below functions are located in kernel_context.S */
noreturn void isr_eret_to_userland(void);
int save_context_and_restore_next(uint64_t curr_tcb, uint64_t next_tcb,
                                  uint32_t* lock_to_unlock, int* clear_child_tid,
                                  uint32_t* critical_section_lock_to_unlock,
                                  void* scheduling_stack);

static LISTP_TYPE(thread) g_thread_list = LISTP_INIT;

/*
 * Guards operations on g_thread_list. This lock should be acquired in two ways:
 *   - if used in interrupt-handler context, must call spinlock_lock()
 *   - if used in normal (interruptible) context, must call spinlock_lock_disable_irq()
 */
static spinlock_t g_thread_list_lock = INIT_SPINLOCK_UNLOCKED;

/* Atomic variable used to kick sched_thread() into action (instead of waiting for some time) */
bool g_kick_sched_thread = false;

/*
 * PSTATE bits of the context we are about to save. x86's `pushfq; pop` captures RFLAGS with IF
 * as-is (the kernel runs with interrupts enabled); aarch64's `mrs daif` captures the EL1 mask
 * state which is "masked" while we execute here, so force the I bit clear to express "IRQs
 * enabled in EL0 context" -- the eret/scheduler-restore paths rely on this to run the resumed
 * thread with timer IRQs unmasked (preemption would otherwise be impossible).
 */
static uint64_t get_pstate_for_saved_context(void) {
    uint64_t result;
    __asm__ volatile("mrs %0, daif" : "=r"(result));
    return result & ~(uint64_t)0x3c0; /* clear D,A,I,F (bits [9:6]) */
}

/* Read the live user TLS register (x86: rdmsr(MSR_IA32_FS_BASE)). Legal from EL1: it is the
 * EL0-accessible register, not the EL1 kernel one (TPIDR_EL1 -- our per-CPU base). */
static uint64_t arm_get_user_tls(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(val));
    return val;
}

static void save_userland_context(struct thread* curr_thread, struct isr_regs* userland_regs) {
    if (!curr_thread)
        return;

    /* Save the live TPIDR_EL0. This is *required* in a way x86's FS_BASE is not: glibc's TLS
     * setup on aarch64 is `msr tpidr_el0` executed by the app at EL0 (there is no arch_prctl
     * analog to trap), so the kernel's cached context.user_tls only learns the app's TCB by
     * sampling the register at switch-out. Every sched_thread* below overwrites TPIDR_EL0
     * with the incoming thread's cached copy, so skipping this capture silently gave the
     * resumed app a NULL TLS base -- its next printf read a TSD slot through 0 and died with
     * a level-2 permission fault at the low-device identity window. */
    curr_thread->context.user_tls = arm_get_user_tls();

    memcpy(curr_thread->context.fpregs, userland_regs->fpregs, g_xsave_size);

    for (size_t i = 0; i < ARRAY_SIZE(userland_regs->regs); i++)
        curr_thread->context.regs[i] = userland_regs->regs[i];

    curr_thread->irq_pseudo_stack.elr  = userland_regs->elr;
    curr_thread->irq_pseudo_stack.spsr = userland_regs->spsr;
    curr_thread->irq_pseudo_stack.sp   = userland_regs->sp;

    curr_thread->context.pc    = (uint64_t)&isr_eret_to_userland;
    /* just like x86 points context.rsp at the iret frame: save_context_and_restore_next()
     * loads SP_EL1 from context.sp and isr_eret_to_userland reads {elr,spsr,sp} from there */
    curr_thread->context.sp    = (uint64_t)&curr_thread->irq_pseudo_stack;
    curr_thread->context.pstate = get_pstate_for_saved_context();
}

static struct thread* find_next_thread(struct thread* curr_thread) {
    assert(spinlock_is_locked(&g_thread_list_lock));

    if (curr_thread && !curr_thread->is_helper) {
        /* move currently executing thread to the back of the list for round robin scheding */
        LISTP_DEL(curr_thread, &g_thread_list, list);
        LISTP_ADD_TAIL(curr_thread, &g_thread_list, list);
    }

    uint32_t cpu_id            = get_per_cpu_data()->cpu_id;

    /* Console input has no application event-loop path in which to process its
     * receive queue, so ensure CPU0's helper cannot be starved by a continuously
     * runnable user thread. VSOCK queues are normally serviced inline from the
     * socket/event path and fall back to this helper when user threads block. */
    if (cpu_id == 0 &&
            __atomic_load_n(&g_console_trigger_bottomhalf, __ATOMIC_ACQUIRE)) {
        assert(get_per_cpu_data()->bottomhalves_thread);
        assert(get_per_cpu_data()->bottomhalves_thread->state != THREAD_BLOCKED);
        return get_per_cpu_data()->bottomhalves_thread;
    }

    struct thread* next_thread = NULL;

    struct thread* thread;
    LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
        if (thread->state != THREAD_RUNNABLE)
            continue;

        size_t cpu_mask_idx        = cpu_id / BITS_IN_TYPE(unsigned long);
        unsigned long cpu_mask_bit = 1UL << (cpu_id % BITS_IN_TYPE(unsigned long));
        if (!(thread->cpu_mask[cpu_mask_idx] & cpu_mask_bit))
            continue;

        if (!next_thread) {
            /* found first runnable thread, mark it as to-be-run next */
            next_thread = thread;
        } else {
            /* found second runnable thread, kick some other CPU to schedule that thread */
            __atomic_store_n(&g_kick_sched_thread, true, __ATOMIC_RELEASE);
            break;
        }
    }

    if (!next_thread && cpu_id == 0) {
        /* CPU0 must periodically handle incoming events (network packets, stdin) */
        assert(get_per_cpu_data()->bottomhalves_thread);
        assert(get_per_cpu_data()->bottomhalves_thread->state != THREAD_BLOCKED);
        next_thread = get_per_cpu_data()->bottomhalves_thread;
    }

    if (!next_thread) {
        /* absolutely no tasks to do */
        assert(get_per_cpu_data()->idle_thread);
        assert(get_per_cpu_data()->idle_thread->state != THREAD_BLOCKED);
        next_thread = get_per_cpu_data()->idle_thread;
    }

    return next_thread;
}

void sched_thread_uninterruptable(struct isr_regs* userland_regs) {
    uint64_t curr_tcb_base     = replace_with_null_if_dummy_gs_base(
                                     (uintptr_t)get_per_cpu_data()->cur_tcb);
    struct thread* curr_thread = curr_tcb_base ? get_thread_ptr(curr_tcb_base) : NULL;

    spinlock_lock(&g_thread_list_lock); /* will be unlocked during save_context */
    struct thread* next_thread = find_next_thread(curr_thread);
    if (curr_thread && curr_thread->state == THREAD_RUNNING)
        curr_thread->state = THREAD_RUNNABLE;
    next_thread->state = THREAD_RUNNING;

    if (next_thread == curr_thread) {
        /* re-scheduled the same thread, no need to save/restore context */
        spinlock_unlock(&g_thread_list_lock);
        return;
    }

    /* We cannot save the current EL1 (kernel) context because we are on the interrupt handler
     * stack which may be clobbered in-between scheduling the current userland thread, and thus the
     * restored EL1 context will have different values on stack. Instead, we manually save the
     * userland context and rewire PC to point to a special "return via eret" assembly. */
    save_userland_context(curr_thread, userland_regs);

    /* restore the user TLS register for the incoming thread (x86: wrmsr(FS_BASE)); the outgoing
     * thread's live value was already saved by save_userland_context() above */
    uint64_t next_user_tls = next_thread->context.user_tls;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(next_user_tls));

    uint64_t next_tcb = (uint64_t)get_gs_base(next_thread);
    save_context_and_restore_next(/*curr_tcb=*/0x0, next_tcb, /*lock_to_unlock=*/NULL,
                                  /*clear_child_tid=*/NULL, &g_thread_list_lock.lock,
                                  /*scheduling_stack=*/NULL);
}

void sched_thread(uint32_t* lock_to_unlock, int* clear_child_tid) {
    uint64_t curr_tcb_base     = replace_with_null_if_dummy_gs_base(
                                     (uintptr_t)get_per_cpu_data()->cur_tcb);
    struct thread* curr_thread = curr_tcb_base ? get_thread_ptr(curr_tcb_base) : NULL;

    spinlock_lock_disable_irq(&g_thread_list_lock); /* will be unlocked during save_context */
    struct thread* next_thread = find_next_thread(curr_thread);
    if (curr_thread && curr_thread->state == THREAD_RUNNING)
        curr_thread->state = THREAD_RUNNABLE;
    next_thread->state = THREAD_RUNNING;

    if (next_thread == curr_thread) {
        /* re-scheduled the same thread, no need to save/restore context */
        spinlock_unlock_enable_irq(&g_thread_list_lock);
        return;
    }

    /* Save the outgoing thread's live user TLS before overwriting it (see the
     * save_userland_context comment for why the kernel cannot see app-side writes any other
     * way); then restore the incoming thread's cached copy (x86: wrmsr(FS_BASE)). */
    if (curr_thread)
        curr_thread->context.user_tls = arm_get_user_tls();
    uint64_t next_user_tls = next_thread->context.user_tls;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(next_user_tls));

    uint64_t next_tcb = (uint64_t)get_gs_base(next_thread);
    save_context_and_restore_next(curr_tcb_base, next_tcb, lock_to_unlock, clear_child_tid,
                                  &g_thread_list_lock.lock, get_per_cpu_data()->scheduling_stack);
}

void sched_thread_wait(int* futex_word, spinlock_t* lock) {
    assert(spinlock_is_locked(lock));
    assert(lock != &g_thread_list_lock);

    /* this order of locks is required to guarantee that we won't miss any wakeup on this futex word
     * (recall that each wakeup grabs g_thread_list_lock) */
    spinlock_lock_disable_irq(&g_thread_list_lock); /* will be unlocked during save_context */
    spinlock_unlock(lock);

    uint64_t curr_tcb_base     = (uintptr_t)get_per_cpu_data()->cur_tcb;
    struct thread* curr_thread = get_thread_ptr(curr_tcb_base);

    curr_thread->state      = THREAD_BLOCKED;
    curr_thread->blocked_on = futex_word;

    struct thread* next_thread = find_next_thread(curr_thread);
    next_thread->state         = THREAD_RUNNING;

    assert(next_thread != curr_thread);

    curr_thread->context.user_tls = arm_get_user_tls();
    uint64_t next_user_tls = next_thread->context.user_tls;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(next_user_tls));

    uint64_t next_tcb = (uint64_t)get_gs_base(next_thread);
    save_context_and_restore_next(curr_tcb_base, next_tcb, /*lock_to_unlock=*/NULL,
                                  /*clear_child_tid=*/NULL, &g_thread_list_lock.lock,
                                  get_per_cpu_data()->scheduling_stack);

    /* now this thread is scheduled back, it means that it was unblocked via wakeup */
    spinlock_lock(lock);
}

static void sched_thread_wakeup_common(int* futex_word) {
    assert(spinlock_is_locked(&g_thread_list_lock));

    bool found = false;
    struct thread* thread;
    LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
        if (thread->state == THREAD_BLOCKED) {
            /* TODO: add loop of iterating through epoll/select futexes as perf optimization */
            if (thread->blocked_on == futex_word) {
                thread->state      = THREAD_RUNNABLE;
                thread->blocked_on = NULL;
                found              = true;
            }
        }
    }

    if (found)
        __atomic_store_n(&g_kick_sched_thread, true, __ATOMIC_RELEASE);
}

void sched_thread_wakeup_uninterruptable(int* futex_word) {
    spinlock_lock(&g_thread_list_lock);
    sched_thread_wakeup_common(futex_word);
    spinlock_unlock(&g_thread_list_lock);
}

void sched_thread_wakeup(int* futex_word) {
    spinlock_lock_disable_irq(&g_thread_list_lock);
    sched_thread_wakeup_common(futex_word);
    spinlock_unlock_enable_irq(&g_thread_list_lock);
}

void sched_thread_add(struct thread* thread) {
    spinlock_lock_disable_irq(&g_thread_list_lock);
    LISTP_ADD_TAIL(thread, &g_thread_list, list);
    spinlock_unlock_enable_irq(&g_thread_list_lock);
}

void sched_thread_remove(struct thread* thread) {
    spinlock_lock_disable_irq(&g_thread_list_lock);
    LISTP_DEL(thread, &g_thread_list, list);
    thread->state      = THREAD_STOPPED;
    thread->blocked_on = NULL;
    spinlock_unlock_enable_irq(&g_thread_list_lock);
}

void sched_thread_set_cpu_affinity(struct thread* thread, unsigned long* cpu_mask,
                                   size_t cpu_mask_len) {
    assert(g_num_cpus >= 1 && g_num_cpus <= MAX_NUM_CPUS);

    spinlock_lock_disable_irq(&g_thread_list_lock);
    memset(thread->cpu_mask, 0, MAX_NUM_CPU_LONGS * 8);
    for (size_t i = 0; i < g_num_cpus; i++) {
        size_t cpu_mask_idx = i / BITS_IN_TYPE(*cpu_mask);
        if (cpu_mask_idx >= cpu_mask_len)
            break;
        if (cpu_mask[cpu_mask_idx] & (1UL << (i % BITS_IN_TYPE(*cpu_mask)))) {
            thread->cpu_mask[cpu_mask_idx] |= 1UL << (i % BITS_IN_TYPE(*cpu_mask));
        }
    }
    spinlock_unlock_enable_irq(&g_thread_list_lock);
}

/* The x86_64 scheduler's interruptible wait (kernel_sched.h); _PalThreadResume() is not
 * implemented on this architecture (pal_threading.c), so the wait is a plain one and nothing
 * interrupts it. */
int sched_thread_wait_interruptible(int* futex_word, spinlock_t* lock) {
    sched_thread_wait(futex_word, lock);
    return 0;
}

void sched_thread_interrupt(struct thread* thread) {
    __UNUSED(thread);
}
