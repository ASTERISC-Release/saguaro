/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Trivial round-robin Single Queue Multiprocessor Scheduler (SQMS) implementation. Takes into
 * account CPU affinity.
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

/* below functions are located in kernel_events.S */
noreturn void isr_iret_to_userland(void);
int save_context_and_restore_next(uint64_t curr_gs_base, uint64_t next_gs_base,
                                  uint32_t* lock_to_unlock, int* clear_child_tid,
                                  uint32_t* critical_section_lock_to_unlock,
                                  void* scheduling_stack, void (*post_switch)(void*),
                                  void* post_switch_arg);

static LISTP_TYPE(thread) g_thread_list = LISTP_INIT;

/*
 * Guards operations on g_thread_list. This lock should be acquired in two ways:
 *   - if used in interrupt-handler context, must call spinlock_lock()
 *   - if used in normal (interruptible) context, must call spinlock_lock_disable_irq()
 */
static spinlock_t g_thread_list_lock = INIT_SPINLOCK_UNLOCKED;
static void (*g_exit_cleanup[MAX_NUM_CPUS])(void*);
static void* g_exit_cleanup_arg[MAX_NUM_CPUS];

/* Atomic variable used to kick sched_thread() into action (instead of waiting for some time) */
bool g_kick_sched_thread = false;

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

static uint64_t get_rflags(void) {
    uint64_t result;
    __asm__ volatile("pushfq; pop %0" : "=r"(result) : : "cc");
    return result;
}

static void save_userland_context(struct thread* curr_thread, struct isr_regs* userland_regs) {
    if (!curr_thread)
        return;

    /* we don't save the FS register because Gramine doesn't use/modify it; only the app uses and
     * modifies it, and so we can just rely that curr_thread->context.user_fsbase is not affected
     * during these context save/restore */

    memcpy(curr_thread->context.fpregs, userland_regs->fpregs, g_xsave_size);

    curr_thread->context.r8  = userland_regs->r8;
    curr_thread->context.r9  = userland_regs->r9;
    curr_thread->context.r10 = userland_regs->r10;
    curr_thread->context.r11 = userland_regs->r11;
    curr_thread->context.r12 = userland_regs->r12;
    curr_thread->context.r13 = userland_regs->r13;
    curr_thread->context.r14 = userland_regs->r14;
    curr_thread->context.r15 = userland_regs->r15;
    curr_thread->context.rdi = userland_regs->rdi;
    curr_thread->context.rsi = userland_regs->rsi;
    curr_thread->context.rbp = userland_regs->rbp;
    curr_thread->context.rbx = userland_regs->rbx;
    curr_thread->context.rdx = userland_regs->rdx;
    curr_thread->context.rax = userland_regs->rax;
    curr_thread->context.rcx = userland_regs->rcx;

    curr_thread->irq_pseudo_stack.rip    = userland_regs->rip;
    curr_thread->irq_pseudo_stack.cs     = userland_regs->cs;
    curr_thread->irq_pseudo_stack.rflags = userland_regs->rflags;
    curr_thread->irq_pseudo_stack.rsp    = userland_regs->rsp;
    curr_thread->irq_pseudo_stack.ss     = userland_regs->ss;

    curr_thread->context.rip    = (uint64_t)&isr_iret_to_userland;
    curr_thread->context.rsp    = (uint64_t)&curr_thread->irq_pseudo_stack;
    curr_thread->context.rflags = get_rflags();
}

static struct thread* find_next_thread(struct thread* curr_thread) {
    assert(spinlock_is_locked(&g_thread_list_lock));

    if (curr_thread && !curr_thread->is_helper) {
        /* move currently executing thread to the back of the list for round robin scheding */
        LISTP_DEL(curr_thread, &g_thread_list, list);
        LISTP_ADD_TAIL(curr_thread, &g_thread_list, list);
    }

    uint32_t cpu_id            = get_per_cpu_data()->cpu_id;

    /* Once a process has its own root, prefer its runnable threads to boot-root helper threads.
     * Helpers stay runnable while idle, so scanning them first can starve a process thread after
     * it yields on an SMP guest. The feature-disabled single-root path keeps the original order. */
    struct thread* process_thread = NULL;
    struct thread* thread;
    LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
        if (thread->state != THREAD_RUNNABLE || thread->is_helper || !thread->address_space ||
                thread->address_space->root_phys == g_pml4_table_base)
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
    if (process_thread) {
        return process_thread;
    }

    /* Do not switch a lone process thread to an always-runnable PAL helper on yield. */
    if (curr_thread && curr_thread->state != THREAD_BLOCKED && !curr_thread->is_helper &&
            curr_thread->address_space &&
            curr_thread->address_space->root_phys != g_pml4_table_base)
        return curr_thread;

    /* Console input has no application event-loop path in which to process its receive queue.
     * When no process-root thread is runnable, let the CPU0 helper drain pending console events. */
    if (cpu_id == 0 &&
            __atomic_load_n(&g_console_trigger_bottomhalf, __ATOMIC_ACQUIRE)) {
        assert(get_per_cpu_data()->bottomhalves_thread);
        assert(get_per_cpu_data()->bottomhalves_thread->state != THREAD_BLOCKED);
        return get_per_cpu_data()->bottomhalves_thread;
    }

    struct thread* next_thread = NULL;

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
    uint64_t curr_gs_base      = replace_with_null_if_dummy_gs_base(rdmsr(MSR_IA32_GS_BASE));
    struct thread* curr_thread = curr_gs_base ? get_thread_ptr(curr_gs_base) : NULL;

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

    /* We cannot save the current ring-0 (kernel) context because we are on the interrupt handler
     * stack which may be clobbered in-between scheduling the current userland thread, and thus the
     * restored ring-0 context will have different values on stack. Instead, we manually save the
     * userland context and rewire RIP to point to a special "return via iret" assembly. */
    save_userland_context(curr_thread, userland_regs);

    switch_to_thread_address_space(next_thread);

    /* it is cumbersome to restore FSBASE in asm, so restore explicitly here */
    /* Diagnostic logging: print FS base values we are about to install and the thread identities.
     * This helps verify whether per-thread user_fsbase differs between threads or gets clobbered.
     */
    {
        uint32_t cpu_id   = get_per_cpu_data()->cpu_id;
        uintptr_t curr_fs = 0, next_fs = next_thread->context.user_fsbase;

#if 0
        /* Try to read current FS via Pal API (best-effort) */
        (void)PalSegmentBaseGet(PAL_SEGMENT_FS, &curr_fs);
        log_always("[sched] cpu=%u sched_out=%p sched_in=%p curr_fs=0x%lx next_fs=0x%lx", cpu_id,
                   (void*)curr_thread, (void*)next_thread, (unsigned long)curr_fs,
                   (unsigned long)next_fs);
#endif
    }
    wrmsr(MSR_IA32_FS_BASE, next_thread->context.user_fsbase);

    uint64_t next_gs_base = (uint64_t)get_gs_base(next_thread);
    save_context_and_restore_next(/*curr_gs_base=*/0x0, next_gs_base, /*lock_to_unlock=*/NULL,
                                  /*clear_child_tid=*/NULL, &g_thread_list_lock.lock,
                                  /*scheduling_stack=*/NULL, /*post_switch=*/NULL,
                                  /*post_switch_arg=*/NULL);
}

void sched_thread(uint32_t* lock_to_unlock, int* clear_child_tid) {
    uint64_t curr_gs_base      = replace_with_null_if_dummy_gs_base(rdmsr(MSR_IA32_GS_BASE));
    struct thread* curr_thread = curr_gs_base ? get_thread_ptr(curr_gs_base) : NULL;

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

    // log_always("Scheduling out thread %p, scheduling in thread %p\n", curr_thread, next_thread);

    /* clear_child_tid belongs to the exiting thread's address space. Clear it before switching
     * roots; the assembly context switch runs after the next root is active. */
    if (clear_child_tid) {
        __atomic_store_n(clear_child_tid, 0, __ATOMIC_RELEASE);
        clear_child_tid = NULL;
    }
    switch_to_thread_address_space(next_thread);

    /* it is cumbersome to restore FSBASE in asm, so restore explicitly here */
    wrmsr(MSR_IA32_FS_BASE, next_thread->context.user_fsbase);

    uint64_t next_gs_base = (uint64_t)get_gs_base(next_thread);
    uint32_t cpu_id = get_per_cpu_data()->cpu_id;
    void (*post_switch)(void*) = g_exit_cleanup[cpu_id];
    void* post_switch_arg = g_exit_cleanup_arg[cpu_id];
    g_exit_cleanup[cpu_id] = NULL;
    g_exit_cleanup_arg[cpu_id] = NULL;
    save_context_and_restore_next(curr_gs_base, next_gs_base, lock_to_unlock, clear_child_tid,
                                  &g_thread_list_lock.lock, get_per_cpu_data()->scheduling_stack,
                                  post_switch, post_switch_arg);
}

void sched_thread_wait(int* futex_word, spinlock_t* lock) {
    assert(spinlock_is_locked(lock));
    assert(lock != &g_thread_list_lock);

    // log_always("Thread %p going to sleep on futex word %p\n",
    //            get_thread_ptr(rdmsr(MSR_IA32_GS_BASE)), futex_word);

    /* this order of locks is required to guarantee that we won't miss any wakeup on this futex word
     * (recall that each wakeup grabs g_thread_list_lock) */
    spinlock_lock_disable_irq(&g_thread_list_lock); /* will be unlocked during save_context */
    spinlock_unlock(lock);

    uint64_t curr_gs_base      = rdmsr(MSR_IA32_GS_BASE);
    struct thread* curr_thread = get_thread_ptr(curr_gs_base);

    curr_thread->state      = THREAD_BLOCKED;
    curr_thread->blocked_on = futex_word;

    struct thread* next_thread = find_next_thread(curr_thread);
    next_thread->state         = THREAD_RUNNING;

    assert(next_thread != curr_thread);

    switch_to_thread_address_space(next_thread);
    wrmsr(MSR_IA32_FS_BASE, next_thread->context.user_fsbase);

    uint64_t next_gs_base = (uint64_t)get_gs_base(next_thread);
    save_context_and_restore_next(curr_gs_base, next_gs_base, /*lock_to_unlock=*/NULL,
                                  /*clear_child_tid=*/NULL, &g_thread_list_lock.lock,
                                  get_per_cpu_data()->scheduling_stack, /*post_switch=*/NULL,
                                  /*post_switch_arg=*/NULL);

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

void sched_thread_set_exit_cleanup(void (*cleanup)(void*), void* arg) {
    uint32_t cpu_id = get_per_cpu_data()->cpu_id;
    assert(cpu_id < MAX_NUM_CPUS);
    assert(!g_exit_cleanup[cpu_id]);
    g_exit_cleanup[cpu_id] = cleanup;
    g_exit_cleanup_arg[cpu_id] = arg;
}

#ifdef __x86_64__
int sched_thread_share_stack(struct memory_address_space* source,
                             struct memory_address_space* new_thread_space,
                             uintptr_t addr, size_t size) {
    int ret = 0;
    bool changed = false;
    spinlock_lock_disable_irq(&g_thread_list_lock);

    struct memory_address_space* boot = memory_address_space_boot();
    if (boot->root_phys != source->root_phys && boot->root_phys != new_thread_space->root_phys) {
        bool range_changed;
        ret = memory_address_space_share_range_noflush(source, boot, addr, size,
                                                       &range_changed);
        changed |= range_changed;
        if (ret < 0)
            goto out;
    }

    if (source->root_phys != new_thread_space->root_phys) {
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
        if (!space || space->root_phys == source->root_phys ||
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

        uintptr_t sched_top = (uintptr_t)cpu->scheduling_stack;
        uintptr_t sched_start = ALIGN_DOWN(sched_top - SCHEDULING_STACK_SIZE, PAGE_SIZE);
        uintptr_t sched_end = ALIGN_UP(sched_top, PAGE_SIZE);
        int ret = memory_address_space_share_range(boot, target, sched_start,
                                                   sched_end - sched_start);
        if (ret < 0)
            return ret;

        uintptr_t irq_start = ALIGN_DOWN((uintptr_t)cpu->interrupt_stack, PAGE_SIZE);
        uintptr_t irq_end = ALIGN_UP((uintptr_t)cpu->interrupt_stack + INTERRUPT_STACK_SIZE,
                                     PAGE_SIZE);
        ret = memory_address_space_share_range(boot, target, irq_start, irq_end - irq_start);
        if (ret < 0)
            return ret;

        uintptr_t xsave_start = ALIGN_DOWN((uintptr_t)cpu->interrupt_xsave_area, PAGE_SIZE);
        uintptr_t xsave_end = ALIGN_UP((uintptr_t)cpu->interrupt_xsave_area +
                                               INTERRUPT_XSAVE_AREA_SIZE,
                                       PAGE_SIZE);
        ret = memory_address_space_share_range(boot, target, xsave_start, xsave_end - xsave_start);
        if (ret < 0)
            return ret;
    }
    return 0;
}
#endif

bool sched_thread_is_stopped(struct thread* thread) {
    spinlock_lock_disable_irq(&g_thread_list_lock);
    bool stopped = thread->state == THREAD_STOPPED;
    spinlock_unlock_enable_irq(&g_thread_list_lock);
    return stopped;
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
