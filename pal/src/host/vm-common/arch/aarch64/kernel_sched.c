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
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_thread.h"
#include "kernel_virtio.h"
#include "kernel_xsave.h"
#include "pal.h"
#include "pal_common.h"
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

/* spinlock_lock_disable_irq(&g_thread_list_lock), except that the waiting is done with the
 * caller's IRQ state, and only the acquisition masks IRQs. The scheduler lock is taken in tight
 * loops (the idle and bottom-half threads, a parent and child handing a process stream back and
 * forth), and the spinlock is not fair: a CPU that waited for it with IRQs masked lost to the
 * others for long stretches and could not answer a TLB-shootdown SGI meanwhile, which stalled the
 * sender and with it the forks at 4 vCPUs ("still waiting for acks (3/4 responded)"). */
static void thread_list_lock_disable_irq(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    while (true) {
        cli();
        if (spinlock_lock_timeout(&g_thread_list_lock, /*iterations=*/0))
            return;
        __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
        while (__atomic_load_n(&g_thread_list_lock.lock, __ATOMIC_RELAXED) != SPINLOCK_UNLOCKED)
            CPU_RELAX();
    }
}

/* Atomic variable used to kick sched_thread() into action (instead of waiting for some time) */
bool g_kick_sched_thread = false;

/* Multiprocess support (`sys.experimental__multiprocess`): set once the frame pool exists. Then
 * save_context_and_restore_next() (kernel_context.S) installs the next thread's page-table root
 * and runs a pending exit cleanup after every switch; without it neither happens and every
 * thread runs in the boot root, as in the single-process VM. */
bool g_sched_switch_roots = false;
static void (*g_exit_cleanup[MAX_NUM_CPUS])(void*);
static void* g_exit_cleanup_arg[MAX_NUM_CPUS];
/* the cleanup taken by the switch in progress on each CPU (run by sched_post_switch()) */
static void (*g_post_switch[MAX_NUM_CPUS])(void*);
static void* g_post_switch_arg[MAX_NUM_CPUS];

static void switch_to_thread_address_space(struct thread* thread) {
    uint64_t root = thread->address_space ? thread->address_space->root_phys
                                          : g_pml4_table_base;
    int ret = memory_address_space_switch_root(root);
    if (ret < 0) {
        log_error("Failed to switch to thread address-space root %p: %s", (void*)root,
                  pal_strerror(ret));
        BUG();
    }
}

/* Called by save_context_and_restore_next() on the scheduling (or interrupt) stack, after the
 * previous context is saved and cur_tcb holds the next thread's TCB. The root must not change
 * while still running on the previous thread's stack, which may be private to its process (the
 * LibOS runs syscalls on the user stack). */
void sched_switch_to_current_thread_root(void);
void sched_switch_to_current_thread_root(void) {
    switch_to_thread_address_space(get_thread_ptr((uintptr_t)get_per_cpu_data()->cur_tcb));
}

/* Called by save_context_and_restore_next() after the switch and after the scheduler lock is
 * released: runs the exit cleanup that sched_thread() took for this switch (it may destroy the
 * previous thread's root and stacks, so it cannot run any earlier). */
void sched_post_switch(void);
void sched_post_switch(void) {
    uint32_t cpu_id = get_per_cpu_data()->cpu_id;
    void (*cleanup)(void*) = g_post_switch[cpu_id];
    void* arg = g_post_switch_arg[cpu_id];
    if (!cleanup)
        return;
    g_post_switch[cpu_id] = NULL;
    g_post_switch_arg[cpu_id] = NULL;
    /* With IRQs unmasked: the cleanup waits for the exiting process's threads that still run on
     * other CPUs (vm_process_reap_threads()), and such a thread may itself wait for this CPU's
     * TLB-shootdown acknowledgement (an munmap in its process): masked, that is a deadlock ("still
     * waiting for acks (3/4 responded)" at 4 vCPUs). An IRQ here only acknowledges, wakes or
     * flushes: from EL1 the tick does not preempt, and the handler runs on the interrupt stack,
     * not on this scheduling stack. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    cleanup(arg);
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static bool thread_is_exiting(struct thread* thread) {
    return thread->address_space && __atomic_load_n(&thread->address_space->exiting,
                                                    __ATOMIC_ACQUIRE)
           && thread->address_space->exit_thread != thread;
}

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
    struct thread* thread;

    if (g_sched_switch_roots) {
        /* (x86 kernel_sched.c verbatim) Once a process has its own root, prefer its runnable
         * threads to boot-root helper threads: helpers stay runnable while idle, so scanning them
         * first can starve a process thread after it yields on an SMP guest. */
        struct thread* process_thread = NULL;
        LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
            if (thread->state != THREAD_RUNNABLE || thread->is_helper || !thread->address_space ||
                    thread->address_space->root_phys == g_pml4_table_base ||
                    thread_is_exiting(thread))
                continue;

            size_t cpu_mask_idx = cpu_id / BITS_IN_TYPE(unsigned long);
            unsigned long cpu_mask_bit = 1UL << (cpu_id % BITS_IN_TYPE(unsigned long));
            if (!(thread->cpu_mask[cpu_mask_idx] & cpu_mask_bit))
                continue;

            if (!process_thread)
                process_thread = thread;
            else
                __atomic_store_n(&g_kick_sched_thread, true, __ATOMIC_RELEASE);
        }
        if (process_thread)
            return process_thread;

        /* Do not switch a lone process thread to an always-runnable PAL helper on yield. */
        if (curr_thread && curr_thread->state != THREAD_BLOCKED && !curr_thread->is_helper &&
                curr_thread->address_space &&
                curr_thread->address_space->root_phys != g_pml4_table_base &&
                !thread_is_exiting(curr_thread))
            return curr_thread;
    }

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

    LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
        if (thread->state != THREAD_RUNNABLE || thread_is_exiting(thread))
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

    thread_list_lock_disable_irq(); /* will be unlocked during save_context */
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
    uint32_t cpu_id = get_per_cpu_data()->cpu_id;
    g_post_switch[cpu_id] = g_exit_cleanup[cpu_id];
    g_post_switch_arg[cpu_id] = g_exit_cleanup_arg[cpu_id];
    g_exit_cleanup[cpu_id] = NULL;
    g_exit_cleanup_arg[cpu_id] = NULL;
    save_context_and_restore_next(curr_tcb_base, next_tcb, lock_to_unlock, clear_child_tid,
                                  &g_thread_list_lock.lock, get_per_cpu_data()->scheduling_stack);
}

void sched_thread_wait(int* futex_word, spinlock_t* lock) {
    assert(spinlock_is_locked(lock));
    assert(lock != &g_thread_list_lock);

    /* this order of locks is required to guarantee that we won't miss any wakeup on this futex word
     * (recall that each wakeup grabs g_thread_list_lock) */
    thread_list_lock_disable_irq(); /* will be unlocked during save_context */
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
    struct thread* tmp;
    LISTP_FOR_EACH_ENTRY_SAFE(thread, tmp, &g_thread_list, list) {
        if (thread->state == THREAD_BLOCKED) {
            /* TODO: add loop of iterating through epoll/select futexes as perf optimization */
            if (thread->blocked_on == futex_word) {
                thread->state      = THREAD_RUNNABLE;
                thread->blocked_on = NULL;
                found              = true;
                /* with processes, a woken thread runs before threads that used their time
                 * slice (a parent and child handing a stream back and forth must not wait a
                 * whole 100-ms slice of a busy thread on every handoff) */
                if (g_sched_switch_roots && !thread->is_helper) {
                    LISTP_DEL(thread, &g_thread_list, list);
                    LISTP_ADD(thread, &g_thread_list, list);
                }
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
    thread_list_lock_disable_irq();
    sched_thread_wakeup_common(futex_word);
    spinlock_unlock_enable_irq(&g_thread_list_lock);
}

void sched_thread_add(struct thread* thread) {
    thread_list_lock_disable_irq();
    LISTP_ADD_TAIL(thread, &g_thread_list, list);
    spinlock_unlock_enable_irq(&g_thread_list_lock);
}

void sched_thread_remove(struct thread* thread) {
    thread_list_lock_disable_irq();
    LISTP_DEL(thread, &g_thread_list, list);
    thread->state      = THREAD_STOPPED;
    thread->blocked_on = NULL;
    spinlock_unlock_enable_irq(&g_thread_list_lock);
}

void sched_thread_set_cpu_affinity(struct thread* thread, unsigned long* cpu_mask,
                                   size_t cpu_mask_len) {
    assert(g_num_cpus >= 1 && g_num_cpus <= MAX_NUM_CPUS);

    thread_list_lock_disable_irq();
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

void sched_thread_set_exit_cleanup(void (*cleanup)(void*), void* arg) {
    uint32_t cpu_id = get_per_cpu_data()->cpu_id;
    assert(cpu_id < MAX_NUM_CPUS);
    assert(!g_exit_cleanup[cpu_id]);
    g_exit_cleanup[cpu_id] = cleanup;
    g_exit_cleanup_arg[cpu_id] = arg;
}

bool sched_thread_is_stopped(struct thread* thread) {
    thread_list_lock_disable_irq();
    bool stopped = thread->state == THREAD_STOPPED;
    spinlock_unlock_enable_irq(&g_thread_list_lock);
    return stopped;
}

/* The rest is the x86 kernel_sched.c multiprocess part, unchanged. */
bool sched_thread_space_has_other_running(struct memory_address_space* space,
                                          struct thread* self) {
    bool running = false;
    thread_list_lock_disable_irq();
    struct thread* thread;
    LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
        if (thread->address_space == space && thread != self && thread->state == THREAD_RUNNING) {
            running = true;
            break;
        }
    }
    spinlock_unlock_enable_irq(&g_thread_list_lock);
    return running;
}

size_t sched_thread_remove_exiting(struct memory_address_space* space, struct thread** out_threads,
                                   size_t max, bool* out_running) {
    size_t count = 0;
    bool running = false;
    thread_list_lock_disable_irq();
    struct thread* thread;
    struct thread* tmp;
    LISTP_FOR_EACH_ENTRY_SAFE(thread, tmp, &g_thread_list, list) {
        if (thread->address_space != space)
            continue;
        if (thread->state == THREAD_RUNNING) {
            /* still on another CPU; find_next_thread() will not pick it again */
            running = true;
            continue;
        }
        if (count == max)
            break;
        LISTP_DEL(thread, &g_thread_list, list);
        thread->state      = THREAD_STOPPED;
        thread->blocked_on = NULL;
        out_threads[count++] = thread;
    }
    spinlock_unlock_enable_irq(&g_thread_list_lock);
    *out_running = running;
    return count;
}

int sched_thread_share_stack(struct memory_address_space* source,
                             struct memory_address_space* new_thread_space,
                             uintptr_t addr, size_t size) {
    int ret = 0;
    bool changed = false;
    thread_list_lock_disable_irq();

    /* On AArch64 the boot root is never a target: its tables are shared with the lazy copies (see
     * memory_address_space_share_range_noflush()), and a stack created before the frame pool
     * exists is already mapped there. */
    if (source->root_phys != new_thread_space->root_phys &&
            new_thread_space != memory_address_space_boot()) {
        bool range_changed;
        ret = memory_address_space_share_range_noflush(source, new_thread_space, addr, size,
                                                       &range_changed);
        changed |= range_changed;
        if (ret < 0)
            goto out;
    }

    struct thread* thread;
    LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
        struct memory_address_space* space = thread->address_space;
        if (!space || space == memory_address_space_boot() ||
                space->root_phys == source->root_phys ||
                space->root_phys == new_thread_space->root_phys)
            continue;
        bool range_changed;
        ret = memory_address_space_share_range_noflush(source, space, addr, size,
                                                       &range_changed);
        changed |= range_changed;
        if (ret < 0)
            goto out;
    }

out:
    spinlock_unlock_enable_irq(&g_thread_list_lock);
    if (changed) {
        int flush_ret = memory_address_space_flush_range(addr, size);
        if (ret >= 0)
            ret = flush_ret;
    }
    return ret;
}

/* The per-CPU helper threads and their stacks were allocated before any process root existed
 * (identity PAL memory); a process root that made the tables over them private must still map
 * them. The interrupt and scheduling stacks come from the kernel image's shared region, which no
 * root remaps. */
int sched_thread_share_runtime_stacks(struct memory_address_space* target) {
    struct memory_address_space* boot = memory_address_space_boot();
    for (uint32_t i = 0; i < g_num_cpus; i++) {
        struct per_cpu_data* cpu = &g_per_cpu_data[i];

        struct thread* helpers[] = {cpu->idle_thread, cpu->bottomhalves_thread};
        for (size_t h = 0; h < ARRAY_SIZE(helpers); h++) {
            struct thread* helper = helpers[h];
            if (!helper)
                continue;
            uintptr_t helper_start = ALIGN_DOWN((uintptr_t)helper, PAGE_SIZE);
            uintptr_t helper_end = ALIGN_UP((uintptr_t)helper + sizeof(*helper), PAGE_SIZE);
            int ret = memory_address_space_share_range(boot, target, helper_start,
                                                       helper_end - helper_start);
            if (ret < 0)
                return ret;

            uintptr_t tcb = get_gs_base(helper);
            uintptr_t stack = tcb - (THREAD_STACK_SIZE + ALT_STACK_SIZE -
                                     sizeof(struct pal_tcb_vm));
            size_t stack_size = THREAD_STACK_SIZE + ALT_STACK_SIZE + g_xsave_size +
                                VM_XSAVE_ALIGN;
            uintptr_t start = ALIGN_DOWN(stack, PAGE_SIZE);
            uintptr_t end = ALIGN_UP(stack + stack_size, PAGE_SIZE);
            ret = memory_address_space_share_range(boot, target, start, end - start);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

/* The x86_64 scheduler's interruptible wait (kernel_sched.h); _PalThreadResume() is not
 * implemented on this architecture (pal_threading.c), so the wait is a plain one and nothing
 * interrupts it. */
int sched_thread_wait_interruptible(int* futex_word, spinlock_t* lock) {
    sched_thread_wait(futex_word, lock);
    return 0;
}

uint64_t g_streams_wake_gen;

void sched_streams_wake(void) {
    __atomic_fetch_add(&g_streams_wake_gen, 1, __ATOMIC_RELEASE);
    sched_thread_wakeup(&g_streams_waiting_events_futex);
}

/* aarch64 keeps the compare before the sleep rather than under the thread-list lock (its
 * sched_thread_wait() has no interruptible variant to extend yet): a waker that runs between
 * the compare and the sleep is still only rescued by the periodic tick, as before; a waker that
 * ran between the caller's sampling and here is caught. */
int sched_thread_wait_interruptible_gen(int* futex_word, spinlock_t* lock, uint64_t* gen,
                                        uint64_t expected) {
    if (gen && __atomic_load_n(gen, __ATOMIC_ACQUIRE) != expected)
        return 1;
    sched_thread_wait(futex_word, lock);
    return 0;
}

void sched_thread_interrupt(struct thread* thread) {
    __UNUSED(thread);
}
