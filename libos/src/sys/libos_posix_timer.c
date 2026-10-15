/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Implementation of the POSIX timers: "timer_create", "timer_settime", "timer_gettime",
 * "timer_getoverrun" and "timer_delete".
 *
 * A POSIX timer is an entry in a per-process table (like the System V IPC objects). A worker
 * thread (created on the first `timer_create()` of this process, same pattern as the timerfd one)
 * computes expirations; on each expiration it adds the overruns to the timer's counter and raises
 * the configured signal with `si_code == SI_TIMER` through the normal signal queues (SIGEV_NONE
 * only counts). Overruns are reported and cleared by `timer_getoverrun()`.
 *
 * SIGEV_THREAD is not supported (returns EINVAL as the kernel does: the notification would need a
 * thread created with the caller's attributes and stack). After `fork()`/checkpoint the worker
 * thread only exists in the process that started it: timers created by the parent simply never
 * fire in the child.
 */

#include "libos_checkpoint.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_utils.h"
#include "linux_abi/time.h"
#include "linux_abi/signals.h"
#include "pal.h"
#include <linux/time.h> /* `MAX_CLOCKS`, `TIMER_ABSTIME`, `struct __kernel_itimerspec` */

#define POSIX_TIMERS_MAX 64

struct posix_timer {
    bool in_use;
    bool armed;
    int clock_id;
    int notify;              /* SIGEV_SIGNAL or SIGEV_NONE */
    int sigev_signo;
    sigval_t sigev_value;
    uint64_t expiry_ns;      /* absolute, in the clock's nanoseconds */
    uint64_t interval_ns;    /* 0 for one-shot */
    unsigned int overrun;    /* expirations not yet reported by `timer_getoverrun()` */
};

/* one expiration to signal, collected by the worker outside the spinlock */
struct posix_timer_signal {
    int signo;
    sigval_t value;
};

static struct posix_timer g_posix_timers[POSIX_TIMERS_MAX];
static spinlock_t g_posix_timers_lock = INIT_SPINLOCK_UNLOCKED;

static PAL_HANDLE g_posix_timer_kick_event;
static struct libos_thread* g_posix_timer_worker_thread;
static bool g_posix_timer_worker_started;
static spinlock_t g_posix_timer_worker_lock = INIT_SPINLOCK_UNLOCKED;

static int posix_timer_now_ns(uint64_t* out_ns) {
    uint64_t time_us = 0;
    int ret = PalSystemTimeQuery(&time_us);
    if (ret < 0)
        return pal_to_unix_errno(ret);
    *out_ns = time_us * (uint64_t)1000;
    return 0;
}

static void posix_timer_kick(void) {
    if (!g_posix_timer_kick_event)
        return;
    uint64_t one = 1;
    size_t size = sizeof(one);
    PalStreamWrite(g_posix_timer_kick_event, /*offset=*/0, &size, &one);
}

static int posix_timer_worker(void* arg) {
    __UNUSED(arg);
    assert(g_posix_timer_worker_thread);

    libos_tcb_init();
    set_cur_thread(g_posix_timer_worker_thread);
    log_setprefix(libos_get_tcb());

    while (true) {
        uint64_t now_ns = 0;
        if (posix_timer_now_ns(&now_ns) < 0) {
            PalThreadYieldExecution();
            continue;
        }

        uint64_t next_ns = UINT64_MAX;
        struct posix_timer_signal to_raise[POSIX_TIMERS_MAX];
        size_t raise_count = 0;

        spinlock_lock(&g_posix_timers_lock);
        for (size_t i = 0; i < POSIX_TIMERS_MAX; i++) {
            struct posix_timer* t = &g_posix_timers[i];
            if (!t->in_use || !t->armed)
                continue;
            if (t->expiry_ns > now_ns) {
                if (t->expiry_ns < next_ns)
                    next_ns = t->expiry_ns;
                continue;
            }

            uint64_t n;
            if (t->interval_ns) {
                n = (now_ns - t->expiry_ns) / t->interval_ns + 1;
                t->expiry_ns += n * t->interval_ns;
                if (t->expiry_ns < next_ns)
                    next_ns = t->expiry_ns;
            } else {
                n = 1;
                t->armed = false;
            }

            /* the overrun counter counts expirations beyond the signal raised now, and
             * saturates at INT_MAX like the kernel's `clamp_int()` (CVE-2018-12896) */
            if (n - 1 >= (uint64_t)INT_MAX - t->overrun)
                t->overrun = (unsigned int)INT_MAX;
            else
                t->overrun += (unsigned int)(n - 1);

            if (t->notify == SIGEV_SIGNAL) {
                if (raise_count < POSIX_TIMERS_MAX) {
                    struct posix_timer_signal s = { .signo = t->sigev_signo,
                                                    .value = t->sigev_value };
                    to_raise[raise_count++] = s;
                }
            } else {
                /* SIGEV_NONE: nothing is delivered, so this expiration is an overrun too */
                if (t->overrun < (unsigned int)INT_MAX)
                    t->overrun += 1;
            }
        }
        spinlock_unlock(&g_posix_timers_lock);

        for (size_t i = 0; i < raise_count; i++) {
            siginfo_t info = {0};
            info.si_signo = to_raise[i].signo;
            info.si_errno = 0;
            info.si_code = SI_TIMER;
            info.si_value = to_raise[i].value;
            /* raise it on the process (any thread that does not block it takes it) */
            append_signal(/*thread=*/NULL, &info);
            libos_signal_wake_waiters(info.si_signo);
        }

        uint64_t timeout_us = 0;
        uint64_t* timeout = NULL;
        if (next_ns != UINT64_MAX && next_ns > now_ns) {
            timeout_us = (next_ns - now_ns + (uint64_t)999) / (uint64_t)1000;
            timeout = &timeout_us;
        }

        pal_wait_flags_t wait_for = PAL_WAIT_READ;
        pal_wait_flags_t ret_events = 0;
        int ret = PalStreamsWaitEvents(1, &g_posix_timer_kick_event, &wait_for, &ret_events,
                                       timeout);
        if (ret == 0 && (ret_events & PAL_WAIT_READ)) {
            uint64_t val = 0;
            size_t size = sizeof(val);
            PalStreamRead(g_posix_timer_kick_event, /*offset=*/0, &size, &val);
        } else if (ret < 0 && ret != -PAL_ERROR_TRYAGAIN) {
            log_warning("posix timer worker: waiting failed: %s", pal_strerror(ret));
            PalThreadYieldExecution();
        }
    }
    BUG();
}

static int posix_timer_start_worker_locked(void) {
    if (g_posix_timer_worker_started)
        return 0;

    int ret = PalStreamOpen(URI_PREFIX_EVENTFD, PAL_ACCESS_RDWR, /*share_flags=*/0,
                            PAL_CREATE_IGNORED, PAL_OPTION_NONBLOCK, &g_posix_timer_kick_event);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    g_posix_timer_worker_thread = get_new_internal_thread();
    if (!g_posix_timer_worker_thread) {
        PalObjectDestroy(g_posix_timer_kick_event);
        g_posix_timer_kick_event = NULL;
        return -ENOMEM;
    }

    PAL_HANDLE pal_handle = NULL;
    ret = PalThreadCreate(posix_timer_worker, NULL, &pal_handle);
    if (ret < 0) {
        put_thread(g_posix_timer_worker_thread);
        g_posix_timer_worker_thread = NULL;
        PalObjectDestroy(g_posix_timer_kick_event);
        g_posix_timer_kick_event = NULL;
        return pal_to_unix_errno(ret);
    }
    g_posix_timer_worker_thread->pal_handle = pal_handle;

    g_posix_timer_worker_started = true;
    return 0;
}

long libos_syscall_timer_create(int clockid, struct sigevent* evp, int* out_timerid) {
    /* the kernel's posix clock registry: only these clocks can own a timer */
    switch (clockid) {
        case CLOCK_REALTIME:
        case CLOCK_MONOTONIC:
        case CLOCK_BOOTTIME:
        case CLOCK_TAI:
            break; /* all are served by the single wall-clock base, as in the timerfd code */
        default:
            if (clockid < 0 || clockid >= MAX_CLOCKS)
                return -EINVAL;
            /* the alarm clocks exist but need CAP_WAKE_ALARM (as in the kernel); the CPU clocks
             * and the unregistered ones are no posix clocks (the kernel's -EOPNOTSUPP) */
            if (clockid == CLOCK_REALTIME_ALARM || clockid == CLOCK_BOOTTIME_ALARM)
                return -EPERM;
            return -EOPNOTSUPP;
    }

    if (!is_user_memory_writable(out_timerid, sizeof(*out_timerid)))
        return -EFAULT;

    /* a NULL `evp` means the default notification: `SIGEV_SIGNAL` of `SIGALRM` (see
     * `timer_create(2)`) */
    int notify = SIGEV_SIGNAL;
    int signo = SIGALRM;
    sigval_t value = {0};
    if (evp) {
        if (!is_user_memory_readable(evp, sizeof(*evp)))
            return -EFAULT;
        notify = evp->sigev_notify;
        signo = evp->sigev_signo;
        value = evp->sigev_value;
    }

    /* the kernel validates the notification type strictly (LTP `timer_create02`/`timer_create03`) */
    if (notify == SIGEV_THREAD_ID) {
        /* like the kernel: only the calling process itself may be targeted */
        if (evp->_sigev_un._tid != (int)g_process.pid)
            return -EINVAL;
        notify = SIGEV_SIGNAL;
    }
    if (notify != SIGEV_NONE && notify != SIGEV_SIGNAL)
        return -EINVAL; /* SIGEV_THREAD (and anything else) is rejected like the kernel does */
    if (notify == SIGEV_SIGNAL && (signo < 1 || signo > SIGS_CNT))
        return -EINVAL;

    spinlock_lock(&g_posix_timer_worker_lock);
    int ret = posix_timer_start_worker_locked();
    spinlock_unlock(&g_posix_timer_worker_lock);
    if (ret < 0)
        return ret;

    spinlock_lock(&g_posix_timers_lock);
    int id = -1;
    for (int i = 0; i < POSIX_TIMERS_MAX; i++) {
        if (!g_posix_timers[i].in_use) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        ret = -EAGAIN; /* the per-process limit (the kernel's is much higher) */
    } else {
        struct posix_timer* t = &g_posix_timers[id];
        memset(t, 0, sizeof(*t));
        t->in_use = true;
        t->armed = false;
        t->clock_id = clockid;
        t->notify = notify;
        t->sigev_signo = signo;
        t->sigev_value = value;
        /* the kernel's timer id is a 32-bit `__kernel_timer_t`: writing a pointer-sized value
         * smashed the caller's stack (LTP timer_delete01) */
        *out_timerid = id + 1;
        ret = 0;
    }
    spinlock_unlock(&g_posix_timers_lock);
    return ret;
}

static struct posix_timer* posix_timer_get(void* timerid) {
    /* the id arrives as a 32-bit `__kernel_timer_t` in a 64-bit register */
    int id = (int)(intptr_t)timerid - 1;
    if (id < 0 || id >= POSIX_TIMERS_MAX || !g_posix_timers[id].in_use)
        return NULL;
    return &g_posix_timers[id];
}

static int kernel_timespec_to_ns(const struct __kernel_timespec* ts, uint64_t* out_ns) {
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
        return -EINVAL;
    uint64_t sec = (uint64_t)ts->tv_sec;
    if (sec > (UINT64_MAX - (uint64_t)ts->tv_nsec) / (uint64_t)1000000000)
        *out_ns = UINT64_MAX;
    else
        *out_ns = sec * (uint64_t)1000000000 + (uint64_t)ts->tv_nsec;
    return 0;
}

static void ns_to_kernel_timespec(uint64_t ns, struct __kernel_timespec* ts) {
    ts->tv_sec = (__kernel_time64_t)(ns / (uint64_t)1000000000);
    ts->tv_nsec = (__kernel_long_t)(ns % (uint64_t)1000000000);
}

long libos_syscall_timer_settime(void* timerid, int flags,
                                 const struct __kernel_itimerspec* new_val,
                                 struct __kernel_itimerspec* old_val) {
    if (flags & ~TIMER_ABSTIME)
        return -EINVAL;

    if (new_val && !is_user_memory_readable(new_val, sizeof(*new_val)))
        return -EFAULT;
    if (old_val && !is_user_memory_writable(old_val, sizeof(*old_val)))
        return -EFAULT;

    uint64_t value_ns = 0;
    uint64_t interval_ns = 0;
    if (new_val) {
        int ret = kernel_timespec_to_ns(&new_val->it_value, &value_ns);
        if (ret < 0)
            return ret;
        ret = kernel_timespec_to_ns(&new_val->it_interval, &interval_ns);
        if (ret < 0)
            return ret;
    }

    uint64_t now_ns = 0;
    int ret = posix_timer_now_ns(&now_ns);
    if (ret < 0)
        return ret;

    bool fire_now = false;
    int fire_signo = 0;
    sigval_t fire_value = {0};
    bool need_kick = false;

    spinlock_lock(&g_posix_timers_lock);
    struct posix_timer* t = posix_timer_get(timerid);
    if (!t) {
        spinlock_unlock(&g_posix_timers_lock);
        return -EINVAL;
    }

    if (old_val) {
        uint64_t remaining = t->armed
                                 ? ((flags & TIMER_ABSTIME) ? t->expiry_ns
                                      : (t->expiry_ns > now_ns ? t->expiry_ns - now_ns : 1))
                                 : 0;
        ns_to_kernel_timespec(remaining, &old_val->it_value);
        ns_to_kernel_timespec(t->interval_ns, &old_val->it_interval);
    }

    if (new_val) {
        t->overrun = 0;
        if (value_ns == 0) {
            t->armed = false;
            t->expiry_ns = 0;
        } else {
            t->expiry_ns = (flags & TIMER_ABSTIME) ? value_ns : now_ns + value_ns;
            t->interval_ns = interval_ns;
            t->armed = true;
            /* an expiration already due at arming time counts now (same rationale as timerfd) */
            if (t->expiry_ns <= now_ns) {
                uint64_t n;
                if (t->interval_ns) {
                    n = (now_ns - t->expiry_ns) / t->interval_ns + 1;
                    t->expiry_ns += n * t->interval_ns;
                } else {
                    n = 1;
                    t->armed = false;
                }
                t->overrun = (n - 1 >= (uint64_t)INT_MAX - t->overrun)
                                 ? (unsigned int)INT_MAX : t->overrun + (unsigned int)(n - 1);
                if (t->notify == SIGEV_SIGNAL) {
                    fire_now = true;
                    fire_signo = t->sigev_signo;
                    fire_value = t->sigev_value;
                }
            }
            need_kick = t->armed;
        }
    }
    spinlock_unlock(&g_posix_timers_lock);

    if (fire_now) {
        /* raise it after dropping the spinlock: the append path takes mutexes */
        siginfo_t info = {0};
        info.si_signo = fire_signo;
        info.si_errno = 0;
        info.si_code = SI_TIMER;
        info.si_value = fire_value;
        append_signal(/*thread=*/NULL, &info);
        /* another thread may already sleep in `sigwaitinfo()` for it */
        libos_signal_wake_waiters(info.si_signo);
    }
    if (need_kick)
        posix_timer_kick();
    return 0;
}

long libos_syscall_timer_gettime(void* timerid, struct __kernel_itimerspec* curr) {
    if (!is_user_memory_writable(curr, sizeof(*curr)))
        return -EFAULT;

    uint64_t now_ns = 0;
    int ret = posix_timer_now_ns(&now_ns);
    if (ret < 0)
        return ret;

    spinlock_lock(&g_posix_timers_lock);
    struct posix_timer* t = posix_timer_get(timerid);
    if (!t) {
        spinlock_unlock(&g_posix_timers_lock);
        return -EINVAL;
    }
    uint64_t remaining = t->armed && t->expiry_ns > now_ns ? t->expiry_ns - now_ns
                     : (t->armed ? 1 : 0);
    ns_to_kernel_timespec(remaining, &curr->it_value);
    ns_to_kernel_timespec(t->interval_ns, &curr->it_interval);
    spinlock_unlock(&g_posix_timers_lock);
    return 0;
}

long libos_syscall_timer_getoverrun(void* timerid) {
    spinlock_lock(&g_posix_timers_lock);
    struct posix_timer* t = posix_timer_get(timerid);
    if (!t) {
        spinlock_unlock(&g_posix_timers_lock);
        return -EINVAL;
    }
    unsigned int overrun = t->overrun;
    t->overrun = 0;
    spinlock_unlock(&g_posix_timers_lock);
    return (long)overrun;
}

long libos_syscall_timer_delete(void* timerid) {
    spinlock_lock(&g_posix_timers_lock);
    struct posix_timer* t = posix_timer_get(timerid);
    if (!t) {
        spinlock_unlock(&g_posix_timers_lock);
        return -EINVAL;
    }
    t->in_use = false;
    t->armed = false;
    spinlock_unlock(&g_posix_timers_lock);
    posix_timer_kick();
    return 0;
}
