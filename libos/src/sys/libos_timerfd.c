/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Implementation of the system calls "timerfd_create", "timerfd_settime" and "timerfd_gettime".
 *
 * The expiration bookkeeping is entirely inside the LibOS, like the default mode of eventfd. A
 * single worker thread (created on the first `timerfd_create()` of this process) computes the next
 * expiration of every armed timerfd and, on each expiration, adds it to the handle's counter and
 * writes a notification to the handle's dummy host eventfd — that host eventfd is what blocking
 * reads and select/poll/epoll wait on (see `fs/timerfd/fs.c`). `timerfd_settime()` wakes the worker
 * through a second (kick) eventfd so that a changed schedule takes effect immediately.
 *
 * All clocks read the system time, exactly like this LibOS's `clock_gettime()`.
 *
 * The worker thread does not survive a `fork()`/checkpoint into the child (no thread is started
 * there); timerfds inherited from the parent are unusable in the child (`checkin` marks them), as
 * for eventfd. Timerfds created in the child get the child's own worker.
 */

#include "libos_checkpoint.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_utils.h"
#include "linux_abi/time.h"
#include <linux/timerfd.h>
#include "pal.h"

#define TIMERFD_MAX_TRACKED 128

/* registry of the armed timerfds of this process; each entry holds a reference to its handle, and
 * the worker thread drops an entry when the timer disarms (one-shot fired, `timerfd_settime()`
 * disarm, or close) */
static struct libos_handle* g_timerfd_tracks[TIMERFD_MAX_TRACKED];
static size_t g_timerfd_tracks_count;
static spinlock_t g_timerfd_tracks_lock = INIT_SPINLOCK_UNLOCKED;

static int timerfd_worker(void* arg);
static PAL_HANDLE g_timerfd_kick_event;              /* wakes the worker early */
static struct libos_thread* g_timerfd_worker_thread; /* bookkeeping only, the thread never exits */
static bool g_timerfd_worker_started;
static spinlock_t g_timerfd_worker_lock = INIT_SPINLOCK_UNLOCKED;

static int timerfd_now_ns(uint64_t* out_ns) {
    uint64_t time_us = 0;
    int ret = PalSystemTimeQuery(&time_us);
    if (ret < 0)
        return pal_to_unix_errno(ret);
    *out_ns = time_us * (uint64_t)1000;
    return 0;
}

static void timerfd_kick(void) {
    if (!g_timerfd_kick_event)
        return;
    uint64_t one = 1;
    size_t size = sizeof(one);
    /* best effort: the worker also wakes up by itself at the next tracked expiration */
    PalStreamWrite(g_timerfd_kick_event, /*offset=*/0, &size, &one);
}

static int timerfd_start_worker_locked(void) {
    if (g_timerfd_worker_started)
        return 0;

    int ret = PalStreamOpen(URI_PREFIX_EVENTFD, PAL_ACCESS_RDWR, /*share_flags=*/0,
                            PAL_CREATE_IGNORED, PAL_OPTION_NONBLOCK, &g_timerfd_kick_event);
    if (ret < 0) {
        log_error("timerfd: failed to create the kick eventfd: %s", pal_strerror(ret));
        return pal_to_unix_errno(ret);
    }

    g_timerfd_worker_thread = get_new_internal_thread();
    if (!g_timerfd_worker_thread) {
        PalObjectDestroy(g_timerfd_kick_event);
        g_timerfd_kick_event = NULL;
        return -ENOMEM;
    }

    PAL_HANDLE pal_handle = NULL;
    ret = PalThreadCreate(timerfd_worker, NULL, &pal_handle);
    if (ret < 0) {
        put_thread(g_timerfd_worker_thread);
        g_timerfd_worker_thread = NULL;
        PalObjectDestroy(g_timerfd_kick_event);
        g_timerfd_kick_event = NULL;
        return pal_to_unix_errno(ret);
    }
    g_timerfd_worker_thread->pal_handle = pal_handle;

    g_timerfd_worker_started = true;
    return 0;
}

/* Write one notification to the handle's dummy host eventfd, unless one is already pending. Called
 * with the caller not holding `t->lock` (a PAL write). */
static void timerfd_fire(struct libos_handle* hdl) {
    struct libos_timerfd_handle* t = &hdl->info.timerfd;

    spinlock_lock(&t->lock);
    bool notify_host = t->dummy_host_val == 0;
    if (notify_host)
        t->dummy_host_val = 1;
    spinlock_unlock(&t->lock);

    if (notify_host) {
        uint64_t one = 1;
        size_t size = sizeof(one);
        PalStreamWrite(hdl->pal_handle, /*offset=*/0, &size, &one);
    }
}

static int timerfd_worker(void* arg) {
    __UNUSED(arg);
    assert(g_timerfd_worker_thread);

    libos_tcb_init();
    set_cur_thread(g_timerfd_worker_thread);
    log_setprefix(libos_get_tcb());

    struct libos_handle* to_put[TIMERFD_MAX_TRACKED];
    struct libos_handle* to_fire[TIMERFD_MAX_TRACKED];

    while (true) {
        uint64_t now_ns = 0;
        if (timerfd_now_ns(&now_ns) < 0) {
            PalThreadYieldExecution();
            continue;
        }

        uint64_t next_ns = UINT64_MAX;
        size_t w = 0;
        size_t put_count = 0;
        size_t fire_count = 0;

        spinlock_lock(&g_timerfd_tracks_lock);
        for (size_t i = 0; i < g_timerfd_tracks_count; i++) {
            struct libos_handle* hdl = g_timerfd_tracks[i];
            struct libos_timerfd_handle* t = &hdl->info.timerfd;
            bool fire = false;

            spinlock_lock(&t->lock);
            if (!t->armed || t->broken_in_child) {
                /* disarmed by `timerfd_settime()`, closed, or inherited into a child: this entry
                 * (and its reference) is no longer needed */
                t->armed = false;
            } else if (t->expiry_ns <= now_ns) {
                uint64_t n;
                if (t->interval_ns) {
                    n = (now_ns - t->expiry_ns) / t->interval_ns + 1;
                    t->expiry_ns += n * t->interval_ns;
                } else {
                    n = 1;
                    t->armed = false; /* a one-shot is done firing */
                }
                if (UINT64_MAX - t->expirations < n)
                    t->expirations = UINT64_MAX;
                else
                    t->expirations += n;
                fire = true;
                if (t->armed && t->expiry_ns < next_ns)
                    next_ns = t->expiry_ns;
            } else {
                if (t->expiry_ns < next_ns)
                    next_ns = t->expiry_ns;
            }
            bool keep = t->armed;
            spinlock_unlock(&t->lock);

            if (keep) {
                g_timerfd_tracks[w++] = hdl;
                if (fire)
                    to_fire[fire_count++] = hdl;
            } else if (fire) {
                /* the one-shot fired its last expiration: notify first (below), then release */
                to_fire[fire_count++] = hdl;
                to_put[put_count++] = hdl;
            } else {
                to_put[put_count++] = hdl;
            }
        }
        g_timerfd_tracks_count = w;
        spinlock_unlock(&g_timerfd_tracks_lock);

        for (size_t i = 0; i < fire_count; i++)
            timerfd_fire(to_fire[i]);
        for (size_t i = 0; i < put_count; i++)
            put_handle(to_put[i]);

        uint64_t timeout_us = 0;
        uint64_t* timeout = NULL;
        if (next_ns != UINT64_MAX && next_ns > now_ns) {
            timeout_us = (next_ns - now_ns + (uint64_t)999) / (uint64_t)1000;
            timeout = &timeout_us;
        }

        pal_wait_flags_t wait_for = PAL_WAIT_READ;
        pal_wait_flags_t ret_events = 0;
        int ret = PalStreamsWaitEvents(1, &g_timerfd_kick_event, &wait_for, &ret_events, timeout);
        if (ret == 0 && (ret_events & PAL_WAIT_READ)) {
            /* a schedule changed: consume the notification, the recomputation above picks it up */
            uint64_t val = 0;
            size_t size = sizeof(val);
            PalStreamRead(g_timerfd_kick_event, /*offset=*/0, &size, &val);
        } else if (ret < 0 && ret != -PAL_ERROR_TRYAGAIN) {
            log_warning("timerfd worker: waiting failed: %s", pal_strerror(ret));
            PalThreadYieldExecution();
        }
    }
    /* not reached */
    BUG();
}

/* Add (or keep) the registry entry for an armed timer; takes the entry's reference on first arm. */
static int timerfd_track(struct libos_handle* hdl) {
    int ret = 0;

    spinlock_lock(&g_timerfd_tracks_lock);
    bool found = false;
    for (size_t i = 0; i < g_timerfd_tracks_count; i++) {
        if (g_timerfd_tracks[i] == hdl) {
            found = true;
            break;
        }
    }
    if (!found) {
        if (g_timerfd_tracks_count == TIMERFD_MAX_TRACKED) {
            ret = -ENOSPC;
        } else {
            get_handle(hdl);
            g_timerfd_tracks[g_timerfd_tracks_count++] = hdl;
        }
    }
    spinlock_unlock(&g_timerfd_tracks_lock);

    timerfd_kick();
    return ret;
}

/* Drop the registry reference, if any (called on disarm and on close). */
int libos_timerfd_untrack(struct libos_handle* hdl) {
    bool found = false;

    spinlock_lock(&g_timerfd_tracks_lock);
    for (size_t i = 0; i < g_timerfd_tracks_count; i++) {
        if (g_timerfd_tracks[i] == hdl) {
            g_timerfd_tracks[i] = g_timerfd_tracks[--g_timerfd_tracks_count];
            found = true;
            break;
        }
    }
    spinlock_unlock(&g_timerfd_tracks_lock);

    if (found) {
        put_handle(hdl);
        timerfd_kick();
    }
    return 0;
}

long libos_syscall_timerfd_create(int clockid, int flags) {
    /* all clocks read the system time, as in `clock_gettime()`; reject what the kernel rejects */
    if (clockid < 0 || clockid >= MAX_CLOCKS)
        return -EINVAL;
    if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK))
        return -EINVAL;

    struct libos_handle* hdl = get_new_handle();
    if (!hdl)
        return -ENOMEM;

    hdl->type = TYPE_TIMERFD;
    hdl->fs = &timerfd_builtin_fs;
    /* the kernel opens a timerfd read-write (writes reach `->write`, which answers EINVAL) */
    hdl->flags = O_RDWR | (flags & TFD_NONBLOCK ? O_NONBLOCK : 0);
    hdl->acc_mode = MAY_READ | MAY_WRITE;

    hdl->info.timerfd.broken_in_child = false;
    hdl->info.timerfd.armed = false;
    hdl->info.timerfd.clock_id = clockid;
    hdl->info.timerfd.expiry_ns = 0;
    hdl->info.timerfd.interval_ns = 0;
    hdl->info.timerfd.expirations = 0;
    hdl->info.timerfd.dummy_host_val = 0;

    spinlock_lock(&g_timerfd_worker_lock);
    int ret = timerfd_start_worker_locked();
    spinlock_unlock(&g_timerfd_worker_lock);
    if (ret < 0)
        goto out;

    /* the dummy host eventfd: notifications for blocking read and for poll/epoll */
    ret = PalStreamOpen(URI_PREFIX_EVENTFD, PAL_ACCESS_RDWR, /*share_flags=*/0,
                        PAL_CREATE_IGNORED, /*flags=*/0, &hdl->pal_handle);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }

    ret = set_new_fd_handle(hdl, flags & TFD_CLOEXEC ? FD_CLOEXEC : 0, NULL);
out:
    put_handle(hdl);
    return ret;
}

static int itimerspec_to_ns(const struct __kernel_timespec* ts, uint64_t* out_ns) {
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
        return -EINVAL;

    uint64_t sec = (uint64_t)ts->tv_sec;
    if (sec > (UINT64_MAX - (uint64_t)ts->tv_nsec) / (uint64_t)1000000000)
        *out_ns = UINT64_MAX;
    else
        *out_ns = sec * (uint64_t)1000000000 + (uint64_t)ts->tv_nsec;
    return 0;
}

static void ns_to_itimerspec(uint64_t ns, struct __kernel_timespec* ts) {
    ts->tv_sec = (__kernel_time_t)(ns / (uint64_t)1000000000);
    ts->tv_nsec = (long)(ns % (uint64_t)1000000000);
}

/* the expired one-shot reports 1ns remaining, like the kernel does */
static void timerfd_report_state(struct libos_timerfd_handle* t, uint64_t now_ns,
                                 bool abstime, struct __kernel_itimerspec* out) {
    uint64_t remaining = 0;
    if (t->armed) {
        if (abstime) {
            remaining = t->expiry_ns;
        } else {
            remaining = t->expiry_ns > now_ns ? t->expiry_ns - now_ns : 1;
        }
    }
    ns_to_itimerspec(remaining, &out->it_value);
    ns_to_itimerspec(t->interval_ns, &out->it_interval);
}

long libos_syscall_timerfd_settime(int fd, int flags,
                                   const struct __kernel_itimerspec* new_val,
                                   struct __kernel_itimerspec* old_val) {
    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
        return -EINVAL;

    /* `TFD_TIMER_CANCEL_ON_SET` needs the realtime clock and clock re-setting notifications;
     * reject it like a clock that has no such notion */
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    int ret = 0;
    if (hdl->type != TYPE_TIMERFD) {
        /* the kernel answers EINVAL for a descriptor that is not a timerfd (the LTP case table
         * checks exactly this) */
        ret = -EINVAL;
        goto out;
    }

    struct libos_timerfd_handle* t = &hdl->info.timerfd;
    if (t->broken_in_child) {
        log_warning("Child process tried to use a timerfd created by its parent");
        ret = -EACCES;
        goto out;
    }

    if ((flags & TFD_TIMER_CANCEL_ON_SET) && t->clock_id != CLOCK_REALTIME) {
        ret = -EINVAL;
        goto out;
    }

    if (new_val && !is_user_memory_readable(new_val, sizeof(*new_val))) {
        ret = -EFAULT;
        goto out;
    }
    if (old_val && !is_user_memory_writable(old_val, sizeof(*old_val))) {
        ret = -EFAULT;
        goto out;
    }

    uint64_t value_ns = 0;
    uint64_t interval_ns = 0;
    if (new_val) {
        ret = itimerspec_to_ns(&new_val->it_value, &value_ns);
        if (ret < 0)
            goto out;
        ret = itimerspec_to_ns(&new_val->it_interval, &interval_ns);
        if (ret < 0)
            goto out;
    }

    uint64_t now_ns = 0;
    ret = timerfd_now_ns(&now_ns);
    if (ret < 0)
        goto out;

    bool armed = false;
    bool fire_now = false;
    spinlock_lock(&t->lock);

    if (old_val)
        timerfd_report_state(t, now_ns, flags & TFD_TIMER_ABSTIME, old_val);

    if (new_val) {
        if (value_ns == 0) {
            t->armed = false;
            t->expirations = 0;
            t->expiry_ns = 0;
        } else {
            t->expiry_ns = (flags & TFD_TIMER_ABSTIME) ? value_ns : now_ns + value_ns;
            t->interval_ns = interval_ns;
            t->expirations = 0;
            t->armed = true;
            /* an expiration already due at arming time counts now, like the kernel's timer
             * interrupt would (the worker could otherwise be a few microseconds late and a
             * nonblocking read would see `EAGAIN`) */
            if (t->expiry_ns <= now_ns) {
                uint64_t n;
                if (t->interval_ns) {
                    n = (now_ns - t->expiry_ns) / t->interval_ns + 1;
                    t->expiry_ns += n * t->interval_ns;
                } else {
                    n = 1;
                    t->armed = false;
                }
                t->expirations = n;
                fire_now = true;
            }
            armed = t->armed;
        }
    }
    spinlock_unlock(&t->lock);

    if (fire_now)
        timerfd_fire(hdl);

    if (new_val) {
        if (armed) {
            ret = timerfd_track(hdl);
        } else {
            libos_timerfd_untrack(hdl);
            ret = 0;
        }
    }
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_timerfd_gettime(int fd, struct __kernel_itimerspec* curr) {
    if (!is_user_memory_writable(curr, sizeof(*curr)))
        return -EFAULT;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    int ret = 0;
    if (hdl->type != TYPE_TIMERFD) {
        /* EINVAL for a descriptor that is not a timerfd, as in `timerfd_settime()` */
        ret = -EINVAL;
        goto out;
    }

    struct libos_timerfd_handle* t = &hdl->info.timerfd;
    if (t->broken_in_child) {
        ret = -EACCES;
        goto out;
    }

    uint64_t now_ns = 0;
    ret = timerfd_now_ns(&now_ns);
    if (ret < 0)
        goto out;

    spinlock_lock(&t->lock);
    timerfd_report_state(t, now_ns, /*abstime=*/false, curr);
    spinlock_unlock(&t->lock);
out:
    put_handle(hdl);
    return ret;
}
