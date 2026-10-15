/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "gettimeofday", "time" and "clock_gettime".
 */

#include "libos_internal.h"
#include "libos_table.h"
#include "linux_abi/errors.h"
#include "pal.h"

long libos_syscall_gettimeofday(struct __kernel_timeval* tv, struct __kernel_timezone* tz) {
    if (tv) {
        if (!is_user_memory_writable(tv, sizeof(*tv)))
            return -EFAULT;

        uint64_t time = 0;
        int ret = PalSystemTimeQuery(&time);
        if (ret < 0) {
            return pal_to_unix_errno(ret);
        }

        tv->tv_sec  = time / 1000000;
        tv->tv_usec = time % 1000000;
    }

    if (tz) {
        if (!is_user_memory_writable(tz, sizeof(*tz)))
            return -EFAULT;

        /* Not implemented, return zeros. */
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }

    return 0;
}

long libos_syscall_time(time_t* tloc) {
    if (tloc && !is_user_memory_writable(tloc, sizeof(*tloc)))
        return -EFAULT;

    uint64_t time = 0;
    int ret = PalSystemTimeQuery(&time);
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }

    time_t t = time / 1000000;

    if (tloc)
        *tloc = t;

    return t;
}

long libos_syscall_clock_gettime(clockid_t which_clock, struct timespec* tp) {
    /* all clocks are the same */
    if (!(0 <= which_clock && which_clock < MAX_CLOCKS))
        return -EINVAL;

    if (!is_user_memory_writable(tp, sizeof(*tp)))
        return -EFAULT;

    if (which_clock == CLOCK_PROCESS_CPUTIME_ID || which_clock == CLOCK_THREAD_CPUTIME_ID) {
        /* the PAL's CPU accounting (`PalGetCpuTime()`, microsecond resolution); a PAL without it
         * (the VM PAL) falls back to the real-time clock as before */
        uint64_t user_us, sys_us;
        if (PalGetCpuTime(which_clock == CLOCK_THREAD_CPUTIME_ID, &user_us, &sys_us) == 0) {
            uint64_t cpu_us = user_us + sys_us;
            tp->tv_sec = cpu_us / 1000000;
            tp->tv_nsec = (cpu_us % 1000000) * 1000;
            return 0;
        }
        if (FIRST_TIME()) {
            log_warning("Per-process and per-thread CPU-time clocks are not supported by this PAL "
                        "in clock_gettime(); they are replaced with system-wide real-time clock.");
        }
    }

    uint64_t time = 0;
    int ret = PalSystemTimeQuery(&time);
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }

    tp->tv_sec  = time / 1000000;
    tp->tv_nsec = (time % 1000000) * 1000;
    return 0;
}

long libos_syscall_clock_getres(clockid_t which_clock, struct timespec* tp) {
    /* all clocks are the same */
    if (!(0 <= which_clock && which_clock < MAX_CLOCKS))
        return -EINVAL;

    if (which_clock == CLOCK_PROCESS_CPUTIME_ID || which_clock == CLOCK_THREAD_CPUTIME_ID) {
        if (FIRST_TIME()) {
            log_warning("Per-process and per-thread CPU-time clocks are not supported in "
                        "clock_getres(); they are replaced with system-wide real-time clock.");
        }
    }

    if (tp) {
        if (!is_user_memory_writable(tp, sizeof(*tp)))
            return -EFAULT;

        tp->tv_sec  = 0;
        tp->tv_nsec = 1000;
    }
    return 0;
}
