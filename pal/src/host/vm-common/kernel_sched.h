/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Declarations for scheduling and context switching. Also takes care of CPU affinity.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spinlock.h"

#include "kernel_interrupts.h"

#define MAX_NUM_CPUS 256
#define MAX_NUM_CPU_LONGS (MAX_NUM_CPUS / BITS_IN_TYPE(unsigned long))
static_assert(MAX_NUM_CPUS % BITS_IN_TYPE(unsigned long) == 0, "");

/*
 * FS_BASE: to allow apps (mainly Glibc) to set up Thread-Local Storage;
 * GS_BASE: used only in kernel mode, points to current TCB of Gramine thread;
 * GS_KERNEL_BASE: used only in kernel mode, points to per-CPU data
 */
#define MSR_IA32_FS_BASE        0xC0000100
#define MSR_IA32_GS_BASE        0xC0000101
#define MSR_IA32_GS_KERNEL_BASE 0xC0000102

extern bool g_kick_sched_thread;

extern int g_streams_waiting_events_futex;

struct thread;
#if defined(__x86_64__) || defined(__aarch64__)
struct memory_address_space;
#endif

/* the four helper functions are implemented in pal_common_threading.c which knows about the
 * relationship between the TCB (which is pointed to by GS base reg) and the thread struct */
struct thread* get_thread_ptr(uintptr_t curr_gs_base);
uintptr_t get_gs_base(struct thread* next_thread);
void set_dummy_gs_base(void);
uintptr_t replace_with_null_if_dummy_gs_base(uintptr_t gs_base);

void sched_thread_uninterruptable(struct isr_regs* userland_regs);
void sched_thread(uint32_t* lock_to_unlock, int* clear_child_tid);
void sched_thread_wait(int* futex_word, spinlock_t* lock);
/* As sched_thread_wait(), for a wait the application may be interrupted in (a blocking read, poll,
 * futex, accept, ...): returns 0 after a wakeup (the caller re-checks its condition) and
 * -PAL_ERROR_INTERRUPTED when sched_thread_interrupt() cut it short or was called before it (the
 * caller returns that to the LibOS, which handles the thread's pending signals). The caller's lock
 * is held again on return either way. */
int sched_thread_wait_interruptible(int* futex_word, spinlock_t* lock);
/* _PalThreadResume(): make `thread` notice a new signal: wake it if it sleeps in an interruptible
 * wait, otherwise let it take the interruption at its next such wait or user-mode timer tick
 * (resume_pending, pal_common_deliver_pending_interrupt()). */
void sched_thread_interrupt(struct thread* thread);
void sched_thread_wakeup_uninterruptable(int* futex_word);
void sched_thread_wakeup(int* futex_word);

void sched_thread_add(struct thread* thread);
void sched_thread_remove(struct thread* thread);
void sched_thread_set_exit_cleanup(void (*cleanup)(void*), void* arg);
#if defined(__x86_64__) || defined(__aarch64__)
int sched_thread_share_stack(struct memory_address_space* source,
                             struct memory_address_space* new_thread_space,
                             uintptr_t addr, size_t size);
int sched_thread_share_runtime_stacks(struct memory_address_space* target);
/* Removes up to `max` threads of an exiting address space that are not running on any CPU and
 * stores them in `out_threads`; `*out_running` tells whether one of its threads still runs. */
bool sched_thread_space_has_other_running(struct memory_address_space* space,
                                          struct thread* self);
size_t sched_thread_remove_exiting(struct memory_address_space* space, struct thread** out_threads,
                                   size_t max, bool* out_running);
#endif
void sched_thread_set_cpu_affinity(struct thread* thread, unsigned long* cpu_mask,
                                   size_t cpu_mask_len);
#if defined(__x86_64__) || defined(__aarch64__)
bool sched_thread_is_stopped(struct thread* thread);
#endif
