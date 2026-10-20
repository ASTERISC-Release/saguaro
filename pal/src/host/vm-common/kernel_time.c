/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Functions for getting time (in us) and setting/triggering timeouts.
 *
 * Notes on multi-core synchronization:
 *   - Timeout operations happen on different CPUs in both normal and interrupt-handling contexts,
 *     sync via timeouts lock
 *   - get_time_in_us()/delay() are thread-safe, don't use global mutable state, no sync required
 */

#include <stdint.h>

#include "api.h"
#include "list.h"
#include "pal_error.h"
#include "spinlock.h"

#include "kernel_sched.h"
#include "kernel_time.h"
#include "kernel_virtio_vsock.h"
#include "kernel_vmm_inputs.h"

#ifdef __x86_64__
/* x86 only: CPU0's timer is armed for the earliest pending deadline (the aarch64 timer keeps the
 * fixed tick; these headers have aarch64 twins that this shared file must not pick up) */
#include "kernel_apic.h"
#include "kernel_multicore.h"
#endif

DEFINE_LIST(pending_timeout);
struct pending_timeout {
    uint64_t timeout_absolute_us;
    int* futex;
    LIST_TYPE(pending_timeout) list;
};
DEFINE_LISTP(pending_timeout);

/* List of pending timeouts; wait-with-timeout events add themselves to this list, and
 * notify_about_timeouts_uninterruptable() callback for timer interrupt walks through this list */
static LISTP_TYPE(pending_timeout) g_pending_timeouts_list = LISTP_INIT;
static spinlock_t g_pending_timeouts_list_lock = INIT_SPINLOCK_UNLOCKED;

#if defined(__x86_64__) || defined(__aarch64__)
/* Absolute time (us) CPU0's timer is armed for, protected by g_pending_timeouts_list_lock: the
 * earliest pending deadline when CPU0's timer ISR last ran, at most the 100 ms tick ahead. A
 * registration of an earlier deadline re-arms the timer (register_timeout()). UINT64_MAX until the
 * first arm, so that the first registration always re-arms. */
static uint64_t g_cpu0_armed_deadline_us = UINT64_MAX;

/* Delay from `now_us` until the earliest pending deadline, clamped to [LAPIC_TIMER_MIN_DELAY_US,
 * LAPIC_TIMER_PERIOD_US]: the tick stays the ceiling (it also kicks the pollers against lost
 * wakeups), the floor bounds the interrupt rate when a deadline is already due. The lock is held. */
static uint64_t next_delay_us_locked(uint64_t now_us) {
    uint64_t delay_us = LAPIC_TIMER_PERIOD_US;
    struct pending_timeout* timeout;
    LISTP_FOR_EACH_ENTRY(timeout, &g_pending_timeouts_list, list) {
        uint64_t d = timeout->timeout_absolute_us > now_us ? timeout->timeout_absolute_us - now_us
                                                           : 0;
        if (d < delay_us)
            delay_us = d;
    }
    return MAX(delay_us, (uint64_t)LAPIC_TIMER_MIN_DELAY_US);
}
#endif

static uint64_t g_start_tsc = 0;
static uint64_t g_start_us  = 0;

extern uint64_t g_tsc_mhz;

/* may return overflow error, but we hope this never happens in real runs */
int get_time_in_us(uint64_t* out_us) {
    assert(g_tsc_mhz);

    uint64_t diff_tsc = get_tsc() - g_start_tsc;
    uint64_t diff_us = diff_tsc / g_tsc_mhz;

    uint64_t us = g_start_us + diff_us;
    if (us < g_start_us)
        return -PAL_ERROR_OVERFLOW;

    *out_us = us;
    return 0;
}

int delay(uint64_t delay_us, bool* continue_gate) {
    uint64_t curr_tsc = get_tsc();
    uint64_t wait_until_tsc = curr_tsc + delay_us * g_tsc_mhz;

    while (curr_tsc < wait_until_tsc) {
        if (continue_gate && __atomic_load_n(continue_gate, __ATOMIC_ACQUIRE))
            break;
        CPU_RELAX();
        curr_tsc = get_tsc();
    }
    return 0;
}

int register_timeout(uint64_t timeout_absolute_us, int* futex, void** timeout_out) {
    if (!timeout_out)
        return -PAL_ERROR_INVAL;

    struct pending_timeout* timeout = malloc(sizeof(*timeout));
    if (!timeout)
        return -PAL_ERROR_NOMEM;

    assert(futex);
    timeout->timeout_absolute_us = timeout_absolute_us;
    timeout->futex = futex;

    /* g_pending_timeouts_list is used by interrupt-handler's
     * notify_about_timeouts_uninterruptable(); temporarily disable interrupts to avoid deadlock */
    spinlock_lock_disable_irq(&g_pending_timeouts_list_lock);
    LISTP_ADD(timeout, &g_pending_timeouts_list, list);
#if defined(__x86_64__) || defined(__aarch64__)
    if (timeout_absolute_us < g_cpu0_armed_deadline_us) {
        /* earlier than what CPU0's timer is armed for: without a re-arm this wait would be served
         * only by the next tick, up to 100 ms late */
        uint64_t now_us;
        if (get_time_in_us(&now_us) == 0) {
#ifdef __aarch64__
            uint64_t delay_us = next_delay_us_locked(now_us);
            g_cpu0_armed_deadline_us = now_us + delay_us;
            lapic_timer_arm_cpu0(delay_us);
#else
            if (get_per_cpu_data()->cpu_id == 0) {
                uint64_t delay_us = next_delay_us_locked(now_us);
                lapic_timer_set_deadline(delay_us);
                g_cpu0_armed_deadline_us = now_us + delay_us;
            } else {
                /* CPU0's timer is per-CPU hardware: make CPU0 take a timer interrupt, its ISR
                 * recomputes the deadline from the list (the stale hardware deadline then fires
                 * one spurious tick, which is harmless) */
                lapic_timer_kick_cpu0();
            }
#endif
        }
    }
#endif
    spinlock_unlock_enable_irq(&g_pending_timeouts_list_lock);

    *timeout_out = (void*)timeout;
    return 0;
}

void deregister_timeout(void* _timeout) {
    struct pending_timeout* timeout = (struct pending_timeout*)_timeout;

    /* g_pending_timeouts_list is used by interrupt-handler's
     * notify_about_timeouts_uninterruptable(); temporarily disable interrupts to avoid deadlock */
    spinlock_lock_disable_irq(&g_pending_timeouts_list_lock);
    struct pending_timeout* list_timeout;
    struct pending_timeout* tmp;
    LISTP_FOR_EACH_ENTRY_SAFE(list_timeout, tmp, &g_pending_timeouts_list, list) {
        if (list_timeout == timeout) {
            LISTP_DEL(timeout, &g_pending_timeouts_list, list);
        }
    }
    spinlock_unlock_enable_irq(&g_pending_timeouts_list_lock);

    free(timeout);
}

int notify_about_timeouts_uninterruptable(void) {
    int ret;
    uint64_t curr_time_us;

    ret = get_time_in_us(&curr_time_us);
    if (ret < 0)
        return ret;

    /* even though we are in non-interruptable ISR context on CPU0, still need to grab
     * g_pending_timeouts_list_lock because normal-context threads on other CPUs may call
     * register_timeout() and deregister_timeout() */
    spinlock_lock(&g_pending_timeouts_list_lock);
    struct pending_timeout* timeout;
    struct pending_timeout* tmp;
    LISTP_FOR_EACH_ENTRY_SAFE(timeout, tmp, &g_pending_timeouts_list, list) {
        if (timeout->timeout_absolute_us <= curr_time_us) {
            sched_thread_wakeup_uninterruptable(timeout->futex);
            LISTP_DEL(timeout, &g_pending_timeouts_list, list);
        }
    }
    /* to prevent lost wakeups on generic g_streams_waiting_events_futex (e.g., when a thread was
     * already running when the wakeup on this futex triggered), need to periodically kick it */
    sched_thread_wakeup_uninterruptable(&g_streams_waiting_events_futex);
    /* the same lost-wakeup class exists for the vsock reader/writer waiters: an incoming packet
     * is moved onto its connection's queue under the connections lock and the wakeup is issued
     * after that lock is dropped, so a thread that rechecks its connection in between and then
     * sleeps misses the wakeup; the queue-availability recheck before sleeping
     * (virtio_vsock_rearm_notifications) cannot see it either, because the packet is no longer
     * in the RX queue. Threads waiting on these futexes are plain sleepers -- no event loop
     * polls on their behalf once their active-polling lease expired -- so they need the same
     * periodic kick. A spurious wakeup is harmless: the waiters recheck and sleep again. */
    sockets_kick_waiting_threads();
    spinlock_unlock(&g_pending_timeouts_list_lock);
    return 0;
}

#if defined(__x86_64__) || defined(__aarch64__)
uint64_t timeouts_next_delay_us(void) {
    uint64_t now_us;
    if (get_time_in_us(&now_us) < 0)
        return LAPIC_TIMER_PERIOD_US;

    /* ISR context on CPU0 (interrupts already disabled), see notify_about_timeouts_uninterruptable() */
    spinlock_lock(&g_pending_timeouts_list_lock);
    uint64_t delay_us = next_delay_us_locked(now_us);
    g_cpu0_armed_deadline_us = now_us + delay_us;
    spinlock_unlock(&g_pending_timeouts_list_lock);
    return delay_us;
}
#endif

int time_init(void) {
    assert(g_tsc_mhz);
    char unixtime_s[TIME_S_STR_MAX];

    g_start_tsc = get_tsc();

    /* Get the UNIX time value on startup from the VMM using "FW CFG" feature of QEMU. Note that
     * this time value is untrusted.
     * TODO: get the UNIX time value from a trusted time source, like a trusted remote server. */
    unixtime_init(unixtime_s, sizeof(unixtime_s));
    uint64_t start_s = atol(unixtime_s);

    /* sanity checks: the obtained untrusted UNIX time must be in a reasonable range e.g., in
     * [1672531200, 1988150400), that is
     * [`TZ=UTC date -d "Jan 1 2023" +%s`, `TZ=UTC date -d "Jan 1 2033" +%s`) */
    if (start_s < 1672531200 || start_s >= 1988150400) {
        return -PAL_ERROR_INVAL;
    }

    g_start_us = start_s * TIME_US_IN_S;

    return 0;
}
