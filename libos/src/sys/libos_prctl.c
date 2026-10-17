/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * prctl(): the options with state the LibOS can keep (the parent-death signal, the thread name,
 * dumpable, keepcaps, no_new_privs, timer slack, the child subreaper flag, THP disable) and the
 * kernel's argument checks for the rest. Unknown options are EINVAL, as in the kernel; a few
 * harmless hints (PR_SET_PTRACER, PR_SET_VMA) are accepted as no-ops. Seccomp is not
 * implemented: PR_SET_SECCOMP performs the kernel's permission checks and then answers EINVAL,
 * the answer of a kernel built without seccomp. The parent-death signal is recorded only (no
 * cross-process delivery).
 */

#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_signal.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "linux_abi/errors.h"

#define PR_SET_PDEATHSIG          1
#define PR_GET_PDEATHSIG          2
#define PR_GET_DUMPABLE           3
#define PR_SET_DUMPABLE           4
#define PR_GET_KEEPCAPS           7
#define PR_SET_KEEPCAPS           8
#define PR_GET_TIMING             13
#define PR_SET_TIMING             14
#define PR_SET_NAME               15
#define PR_GET_NAME               16
#define PR_GET_SECCOMP            21
#define PR_SET_SECCOMP            22
#define PR_CAPBSET_READ           23
#define PR_CAPBSET_DROP           24
#define PR_GET_SECUREBITS         27
#define PR_SET_SECUREBITS         28
#define PR_SET_TIMERSLACK         29
#define PR_GET_TIMERSLACK         30
#define PR_SET_CHILD_SUBREAPER    36
#define PR_GET_CHILD_SUBREAPER    37
#define PR_SET_NO_NEW_PRIVS       38
#define PR_GET_NO_NEW_PRIVS       39
#define PR_GET_TID_ADDRESS        40
#define PR_SET_THP_DISABLE        41
#define PR_GET_THP_DISABLE        42
#define PR_CAP_AMBIENT            47
#define PR_GET_SPECULATION_CTRL   52
#define PR_SET_VMA                0x53564d41
#define PR_SET_PTRACER            0x59616d61

#define PR_CAP_AMBIENT_IS_SET     1
#define PR_CAP_AMBIENT_CLEAR_ALL  4
#define SECCOMP_MODE_STRICT       1
#define SECCOMP_MODE_FILTER       2
#define LIBOS_CAP_LAST_CAP        40
#define LIBOS_CAP_SETPCAP         8
#define LIBOS_CAP_SYS_ADMIN       21
#define DEFAULT_TIMER_SLACK_NS    50000

static int g_dumpable = 1; /* SUID_DUMP_USER */
static bool g_child_subreaper;
static bool g_thp_disable;

static long name_of_thread(struct libos_thread* t, char out[16]) {
    memset(out, 0, 16);
    lock(&t->lock);
    bool set = t->comm[0] != '\0';
    if (set)
        memcpy(out, t->comm, 16);
    unlock(&t->lock);
    if (!set) {
        lock(&g_process.fs_lock);
        if (g_process.exec && g_process.exec->dentry) {
            size_t len = g_process.exec->dentry->name_len;
            memcpy(out, g_process.exec->dentry->name, len > 15 ? 15 : len);
        }
        unlock(&g_process.fs_lock);
    }
    return 0;
}

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#define PR_SYS_DISPATCH_OFF          0
#define PR_SYS_DISPATCH_ON           1
#endif

long libos_syscall_prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4,
                         unsigned long arg5) {
    struct libos_thread* cur = get_cur_thread();

    switch (option) {
        case PR_SET_PDEATHSIG:
            if (arg2 > SIGS_CNT)
                return -EINVAL;
            lock(&cur->lock);
            cur->pdeath_signal = (int)arg2;
            unlock(&cur->lock);
            return 0;
        case PR_GET_PDEATHSIG: {
            int* out = (int*)arg2;
            if (!is_user_memory_writable(out, sizeof(*out)))
                return -EFAULT;
            lock(&cur->lock);
            *out = cur->pdeath_signal;
            unlock(&cur->lock);
            return 0;
        }
        case PR_GET_DUMPABLE:
            return __atomic_load_n(&g_dumpable, __ATOMIC_RELAXED);
        case PR_SET_DUMPABLE:
            if (arg2 != 0 && arg2 != 1)
                return -EINVAL;
            __atomic_store_n(&g_dumpable, (int)arg2, __ATOMIC_RELAXED);
            return 0;
        case PR_GET_KEEPCAPS:
            return cur->keepcaps;
        case PR_SET_KEEPCAPS:
            if (arg2 > 1)
                return -EINVAL;
            cur->keepcaps = arg2 == 1;
            return 0;
        case PR_GET_TIMING:
            return 0; /* PR_TIMING_STATISTICAL */
        case PR_SET_TIMING:
            return arg2 == 0 ? 0 : -EINVAL; /* only PR_TIMING_STATISTICAL exists */
        case PR_SET_SYSCALL_USER_DISPATCH: {
            /* arg2: PR_SYS_DISPATCH_ON/OFF; arg3, arg4: the range [offset, offset + len) whose
             * system calls always go through; arg5: the selector byte's address (or NULL) */
            if (arg2 == PR_SYS_DISPATCH_OFF) {
                if (arg3 || arg4 || arg5)
                    return -EINVAL;
                memset(&cur->syscall_user_dispatch, 0, sizeof(cur->syscall_user_dispatch));
                return 0;
            }
            if (arg2 != PR_SYS_DISPATCH_ON)
                return -EINVAL;
            if (arg3 && arg3 + arg4 <= arg3)
                return -EINVAL;
            if (arg5 && !is_user_memory_readable((void*)arg5, 1))
                return -EFAULT;
            cur->syscall_user_dispatch.start    = arg3;
            cur->syscall_user_dispatch.len      = arg4;
            cur->syscall_user_dispatch.selector = (char*)arg5;
            __atomic_store_n(&cur->syscall_user_dispatch.on, true, __ATOMIC_RELEASE);
            return 0;
        }
        case PR_SET_NAME: {
            const char* name = (const char*)arg2;
            char buf[16] = {0};
            for (size_t i = 0; i < sizeof(buf) - 1; i++) {
                if (!is_user_memory_readable(name + i, 1))
                    return -EFAULT;
                buf[i] = name[i];
                if (!buf[i])
                    break;
            }
            lock(&cur->lock);
            memcpy(cur->comm, buf, sizeof(buf));
            unlock(&cur->lock);
            return 0;
        }
        case PR_GET_NAME: {
            char* out = (char*)arg2;
            if (!is_user_memory_writable(out, 16))
                return -EFAULT;
            char buf[16];
            name_of_thread(cur, buf);
            memcpy(out, buf, 16);
            return 0;
        }
        case PR_GET_SECCOMP:
            return 0; /* no seccomp mode */
        case PR_SET_SECCOMP:
            if (arg2 == SECCOMP_MODE_FILTER) {
                /* struct sock_fprog: an unsigned short count and a pointer */
                if (!is_user_memory_readable((void*)arg3, 16))
                    return -EFAULT;
                if (!cur->no_new_privs && !libos_capable(LIBOS_CAP_SYS_ADMIN))
                    return -EACCES;
            } else if (arg2 != SECCOMP_MODE_STRICT) {
                return -EINVAL;
            }
            return -EINVAL; /* seccomp itself is not implemented */
        case PR_CAPBSET_READ:
            return arg2 <= LIBOS_CAP_LAST_CAP ? 1 : -EINVAL;
        case PR_CAPBSET_DROP:
            if (!libos_capable(LIBOS_CAP_SETPCAP))
                return -EPERM;
            return arg2 <= LIBOS_CAP_LAST_CAP ? 0 : -EINVAL;
        case PR_GET_SECUREBITS:
            return 0;
        case PR_SET_SECUREBITS:
            return libos_capable(LIBOS_CAP_SETPCAP) ? (arg2 == 0 ? 0 : -EINVAL) : -EPERM;
        case PR_SET_TIMERSLACK:
            cur->timer_slack_ns = arg2;
            return 0;
        case PR_GET_TIMERSLACK:
            return cur->timer_slack_ns ? (long)cur->timer_slack_ns : DEFAULT_TIMER_SLACK_NS;
        case PR_SET_CHILD_SUBREAPER:
            g_child_subreaper = arg2 != 0;
            return 0;
        case PR_GET_CHILD_SUBREAPER: {
            int* out = (int*)arg2;
            if (!is_user_memory_writable(out, sizeof(*out)))
                return -EFAULT;
            *out = g_child_subreaper;
            return 0;
        }
        case PR_SET_NO_NEW_PRIVS:
            if (arg2 != 1 || arg3 || arg4 || arg5)
                return -EINVAL;
            cur->no_new_privs = true;
            return 0;
        case PR_GET_NO_NEW_PRIVS:
            if (arg2 || arg3 || arg4 || arg5)
                return -EINVAL;
            return cur->no_new_privs;
        case PR_GET_TID_ADDRESS: {
            int** out = (int**)arg2;
            if (!is_user_memory_writable(out, sizeof(*out)))
                return -EFAULT;
            *out = cur->clear_child_tid;
            return 0;
        }
        case PR_SET_THP_DISABLE:
            if (arg3 || arg4 || arg5)
                return -EINVAL;
            g_thp_disable = arg2 != 0;
            return 0;
        case PR_GET_THP_DISABLE:
            if (arg2 || arg3 || arg4 || arg5)
                return -EINVAL;
            return g_thp_disable;
        case PR_CAP_AMBIENT:
            if (arg2 == PR_CAP_AMBIENT_CLEAR_ALL)
                return (arg3 || arg4 || arg5) ? -EINVAL : 0;
            if (arg2 < PR_CAP_AMBIENT_IS_SET || arg2 > PR_CAP_AMBIENT_CLEAR_ALL || arg4 || arg5 ||
                    arg3 > LIBOS_CAP_LAST_CAP)
                return -EINVAL;
            if (arg2 == PR_CAP_AMBIENT_IS_SET)
                return 0; /* no ambient capabilities */
            return -EPERM; /* raising or lowering needs the capability in permitted+inheritable */
        case PR_GET_SPECULATION_CTRL:
            if (arg2 > 2 || arg3 || arg4 || arg5)
                return -EINVAL;
            return 0; /* PR_SPEC_NOT_AFFECTED */
        case PR_SET_VMA:
        case PR_SET_PTRACER:
            return 0; /* hints without an effect here */
        default:
            return -EINVAL;
    }
}
