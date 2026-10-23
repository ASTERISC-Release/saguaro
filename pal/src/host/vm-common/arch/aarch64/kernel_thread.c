/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * aarch64 counterpart of vm-common/kernel_thread.c.
 *
 * The stack-recycling logic, the idle loop and the bottomhalves loop are architecture-neutral
 * and copied verbatim; only `thread_setup()` differs: it initializes the initial register
 * context for a new kernel thread, which on aarch64 means the `context.regs[]`-style layout
 * (aarch64 branch of struct thread_context) with the entry trampoline in `pc`, the wrapper's
 * three arguments in x0/x1/x2, and a 16-byte-aligned `sp` (no x86-style "8-odd at entry").
 */

#include <stdint.h>

#include "api.h"
#include "asan.h"
#include "spinlock.h"

#include "kernel_sched.h"
#include "kernel_thread.h"
#include "kernel_time.h"
#include "kernel_virtio.h"
#include "kernel_xsave.h"

/* below functions are located in kernel_context.S and pal_common_threading.c */
noreturn void pal_common_thread_exit(int* clear_child_tid);
noreturn void thread_main_wrapper(void* callback_args, void* callback, void* terminate_func);

/* We cannot just use malloc/free to allocate/free thread stacks because thread-exit routine needs
 * to execute on the stack and thus can't execute free (it would execute with no stack after such
 * call). Thus, we resort to recycling thread stacks (and fpregs memory regions allocated together
 * with the stack) allocated by previous threads and not used anymore. This still leaks memory but
 * at least it is bounded by the max number of simultaneously executing threads. */
struct thread_stack_map_t {
    void* stack;
    bool  used;
};

static struct thread_stack_map_t* g_thread_stack_map = NULL;
static size_t g_thread_stack_num  = 0;
static size_t g_thread_stack_size = 0;
static spinlock_t g_thread_stack_lock = INIT_SPINLOCK_UNLOCKED;

int thread_get_stack_and_fpregs(void** out_stack, void** out_fpregs) {
    int ret;
    void* stack_base = NULL;

    /* we allocate both the stack and the fpregs (FP/SIMD) memory region in one go; note that
     * fpregs may be allocated not at VM_XSAVE_ALIGN boundary, so need to add a margin for that */
    assert(g_xsave_size);
    size_t stack_and_fpregs_size = THREAD_STACK_SIZE + ALT_STACK_SIZE + g_xsave_size
                                                     + VM_XSAVE_ALIGN;

    spinlock_lock(&g_thread_stack_lock);
    for (size_t i = 0; i < g_thread_stack_num; i++) {
        if (!g_thread_stack_map[i].used) {
            /* found allocated and unused stack + fpregs -- use it */
            g_thread_stack_map[i].used = true;
            stack_base = g_thread_stack_map[i].stack;
            ret = 0;
            goto out;
        }
    }

    if (g_thread_stack_num == g_thread_stack_size) {
        /* realloc g_thread_stack_map to accommodate more objects (includes the very first time) */
        g_thread_stack_size += 8;
        struct thread_stack_map_t* tmp = malloc(g_thread_stack_size * sizeof(*tmp));
        if (!tmp) {
            ret = -PAL_ERROR_NOMEM;
            goto out;
        }

        memcpy(tmp, g_thread_stack_map, g_thread_stack_num * sizeof(*tmp));
        free(g_thread_stack_map);
        g_thread_stack_map = tmp;
    }

    stack_base = malloc(stack_and_fpregs_size);
    if (!stack_base) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }

    g_thread_stack_map[g_thread_stack_num].stack = stack_base;
    g_thread_stack_map[g_thread_stack_num].used  = true;
    g_thread_stack_num++;

    ret = 0;

out:
    if (ret == 0) {
        assert(stack_base);
#ifdef ASAN
        asan_unpoison_region((uintptr_t)stack_base, stack_and_fpregs_size);
#endif
        *out_stack  = stack_base;
        *out_fpregs = stack_base + THREAD_STACK_SIZE + ALT_STACK_SIZE;
    }
    spinlock_unlock(&g_thread_stack_lock);
    return ret;
}

void thread_release_stack(void* thread_stack) {
    spinlock_lock(&g_thread_stack_lock);
    for (size_t i = 0; i < g_thread_stack_num; i++) {
        if (g_thread_stack_map[i].stack == thread_stack) {
            g_thread_stack_map[i].used = false;
            break;
        }
    }
    spinlock_unlock(&g_thread_stack_lock);
}

noreturn void thread_free_stack_and_die(void* thread_stack, int* clear_child_tid) {
    /* we do not free thread stack (and fpregs memory region allocated with it) but instead mark it
     * as recycled, see thread_get_stack_and_fpregs() */
    spinlock_lock(&g_thread_stack_lock);
    for (size_t i = 0; i < g_thread_stack_num; i++) {
        if (g_thread_stack_map[i].stack == thread_stack) {
            g_thread_stack_map[i].used = false;
            break;
        }
    }

    /* we do not ASan-unpoison the current stack at this point because we call into `sched_thread()`
     * which in turn calls into other functions, i.e. the stack will be used extensively there;
     * instead we ASan-unpoison this stack when it is re-used, see thread_get_stack_and_fpregs() */

    /* we might still be using the stack we just marked as unused until we enter the asm mode,
     * so we do not unlock now but rather when another thread is scheduled */
    set_dummy_gs_base();
    sched_thread(&g_thread_stack_lock.lock, clear_child_tid);
    __builtin_unreachable();
}

void thread_setup(struct thread* thread, void* fpregs, void* stack, int (*callback)(void*),
                  const void* param) {
    memset(fpregs, 0, g_xsave_size + VM_XSAVE_ALIGN);
    memset(stack, 0, THREAD_STACK_SIZE + ALT_STACK_SIZE);

    /* fpregs may have been allocated not at VM_XSAVE_ALIGN boundary, so need to adjust */
    memcpy(ALIGN_UP_PTR(fpregs, VM_XSAVE_ALIGN), &g_xsave_reset_state, VM_XSAVE_RESET_STATE_SIZE);

    /* the context (GPRs, FP area pointer, etc.) is initialized with zeros; set only required regs */
    thread->context.pstate = 0;                              /* EL0t, IRQ enabled (SPSR_EL1) */
    thread->context.pc = (uint64_t)thread_main_wrapper;      /* wrapper func to start */
    thread->context.regs[0] = (uint64_t)param;               /* argument to func */
    thread->context.regs[1] = (uint64_t)callback;            /* actual func to start */
    thread->context.regs[2] = (uint64_t)pal_common_thread_exit; /* wrapper func to terminate */
    thread->context.sp = (uint64_t)stack + THREAD_STACK_SIZE; /* stack top, 16B-aligned */
    thread->context.fpregs = ALIGN_UP_PTR(fpregs, VM_XSAVE_ALIGN);

    /* sigreturn_pseudo_rsp always points at the top of the pseudo stack used in sigreturn flow;
     * see also kernel_context.S */
    thread->sigreturn_pseudo_rsp = (uint64_t)&thread->sigreturn_pseudo_stack;
    thread->sigreturn_pseudo_rsp += sizeof(thread->sigreturn_pseudo_stack);

    static uint32_t thread_id = 0;
    thread->thread_id = __atomic_add_fetch(&thread_id, 1, __ATOMIC_SEQ_CST);

    thread->state = THREAD_RUNNABLE;
    thread->blocked_on = NULL;

    memset(thread->cpu_mask, 0xFF, MAX_NUM_CPU_LONGS * 8);

    __atomic_store_n(&g_kick_sched_thread, true, __ATOMIC_RELEASE);
}

/* helper threads are per-core idle and bottomhalves threads; they are never terminated and thus
 * their resources are never freed */
int thread_helper_create(int (*callback)(void*), struct thread** out_thread) {
    struct thread* thread = calloc(1, sizeof(*thread));
    if (!thread)
        return -PAL_ERROR_NOMEM;

    /* allocate both the stack and the fpregs (FP/SIMD) memory region in one go; note that
     * fpregs may be allocated not at VM_XSAVE_ALIGN boundary, so need to add a margin for that */
    assert(g_xsave_size);
    void* stack_base = malloc(THREAD_STACK_SIZE + ALT_STACK_SIZE + g_xsave_size + VM_XSAVE_ALIGN);
    if (!stack_base) {
        free(thread);
        return -PAL_ERROR_NOMEM;
    }
    void* stack  = stack_base;
    void* fpregs = stack_base + THREAD_STACK_SIZE + ALT_STACK_SIZE;

    thread_setup(thread, fpregs, stack, callback, /*param=*/NULL);
    thread->is_helper = true;

    *out_thread = thread;
    return 0;
}

/* Idle thread (aka idle process) that runs when all other threads are blocked; can happen e.g. when
 * other threads wait on timer interrupt or external-event interrupt */
noreturn int thread_idle_run(void* args) {
    __UNUSED(args);

    while (true) {
        delay(IDLE_THREAD_PERIOD_US, &g_kick_sched_thread);
        __atomic_store_n(&g_kick_sched_thread, false, __ATOMIC_RELEASE);
        sched_thread(/*lock_to_unlock=*/NULL, /*clear_child_tid=*/NULL);
    }

    __builtin_unreachable();
}

/* The bottom-half thread is what CPU0 runs when it has nothing else to run, so its loop was a
 * tight loop through sched_thread(): millions of acquisitions of the scheduler lock per second, with
 * IRQs masked for each list walk, against which a CPU that wanted the lock to wake a thread lost
 * again and again (1,000 forks: 25 s or, in most runs, 100 to 480 s at 2 and 4 vCPUs; FINDINGS.md,
 * phase 4 part C). Between two rounds without work it now waits up to BOTTOMHALVES_BACKOFF_US for a
 * reason to look again: a thread became runnable (g_sched_runnable_gen changed) or a virtio IRQ set
 * a bottom-half trigger. Not while vsock rings are being polled (g_vsock_polling): that polling is
 * the work. */
#define BOTTOMHALVES_BACKOFF_US 50
static void bottomhalves_backoff(void) {
    extern uint64_t g_tsc_mhz;
    extern uint64_t g_sched_runnable_gen;
    if (__atomic_load_n(&g_vsock_polling, __ATOMIC_ACQUIRE))
        return;
    uint64_t gen = __atomic_load_n(&g_sched_runnable_gen, __ATOMIC_ACQUIRE);
    uint64_t until = get_tsc() + BOTTOMHALVES_BACKOFF_US * g_tsc_mhz;
    while (get_tsc() < until) {
        if (__atomic_load_n(&g_sched_runnable_gen, __ATOMIC_ACQUIRE) != gen ||
                __atomic_load_n(&g_vsock_trigger_bottomhalf, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&g_console_trigger_bottomhalf, __ATOMIC_ACQUIRE))
            return;
        CPU_RELAX();
    }
}

/* Thread that performs heavy tasks triggered on IRQs in normal context; runs only on CPU0 */
noreturn int thread_bottomhalves_run(void* args) {
    __UNUSED(args);

    while (true) {
        bool vsock_trigger = !!__atomic_exchange_n(&g_vsock_trigger_bottomhalf, false,
                                                   __ATOMIC_ACQ_REL);
        bool console_trigger = !!__atomic_exchange_n(&g_console_trigger_bottomhalf, false,
                                                     __ATOMIC_ACQ_REL);

        if (vsock_trigger)
            (void)virtio_vsock_bottomhalf();
        else if (__atomic_load_n(&g_vsock_polling, __ATOMIC_ACQUIRE)) {
            if (!virtio_vsock_poll_if_active() && virtio_vsock_rearm_notifications())
                (void)virtio_vsock_bottomhalf();
        }
        if (console_trigger)
            (void)virtio_console_bottomhalf();

        if (!vsock_trigger && !console_trigger)
            bottomhalves_backoff();
        sched_thread(/*lock_to_unlock=*/NULL, /*clear_child_tid=*/NULL);
    }

    __builtin_unreachable();
}
