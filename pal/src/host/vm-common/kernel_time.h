/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/* Declarations for time and timeouts. */

#pragma once

#include <stdint.h>

#define LAPIC_TIMER_PERIOD_US (100 * 1000) /* 100 ms, same as default SCHED_RR interval in Linux */
#define IDLE_THREAD_PERIOD_US (10 * 1000)  /* 10 ms, chosen experimentally */

/* the max of Unix time in seconds is `UINT64_MAX`, which has 20 digits in decimal */
#define TIME_S_STR_MAX (20 + 1)

int get_time_in_us(uint64_t* out_us);
int delay(uint64_t delay_us, bool* continue_gate);

/* `timeout_out` is an opaque object to be used in `deregister_timeout()`. It is the responsibility
 * of the caller to remove the timeout (even if the timeout was already triggered by
 * `notify_about_timeouts_uninterruptable`). */
int register_timeout(uint64_t timeout_absolute_us, int* futex, void** timeout_out);
void deregister_timeout(void* timeout);
/* 0 until the timer ISR fired `timeout` (it then wakes only the threads already blocked on the
 * futex and drops the timeout): a waiter that registered a timeout passes this flag as the
 * generation of sched_thread_wait_interruptible_gen() so that it never blocks after the firing */
uint64_t* timeout_fired_flag(void* timeout);

int remove_timeouts_on_futex(int* futex);
int notify_about_timeouts_uninterruptable(void);

#if defined(__x86_64__) || defined(__aarch64__)
/* the floor of CPU0's timer delay: bounds the interrupt rate when a deadline is already due */
#define LAPIC_TIMER_MIN_DELAY_US 50
/* For CPU0's timer ISR, after notify_about_timeouts_uninterruptable(): the delay to arm the timer
 * with, from now to the earliest pending deadline, at most LAPIC_TIMER_PERIOD_US (the tick stays
 * the ceiling). Records what the timer is armed for, so register_timeout() can re-arm early. */
uint64_t timeouts_next_delay_us(void);
#endif

#ifdef __aarch64__
/* Implemented in arch/aarch64/kernel_apic.c. Called by register_timeout() with the timeouts lock
 * held and IRQs disabled when the new deadline is earlier than what CPU0's timer is armed for:
 * arms CPU0's timer `delay_us` from now (directly on CPU0, by an SGI from another CPU, where it
 * is recomputed from the list). */
void lapic_timer_arm_cpu0(uint64_t delay_us);
#endif

int time_init(void);
