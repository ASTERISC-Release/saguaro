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

int remove_timeouts_on_futex(int* futex);
int notify_about_timeouts_uninterruptable(void);

#ifdef __x86_64__
/* the floor of CPU0's timer delay: bounds the interrupt rate when a deadline is already due */
#define LAPIC_TIMER_MIN_DELAY_US 50
/* For CPU0's timer ISR, after notify_about_timeouts_uninterruptable(): the delay to arm the timer
 * with, from now to the earliest pending deadline, at most LAPIC_TIMER_PERIOD_US (the tick stays
 * the ceiling). Records what the timer is armed for, so register_timeout() can re-arm early. */
uint64_t timeouts_next_delay_us(void);
#endif

int time_init(void);
