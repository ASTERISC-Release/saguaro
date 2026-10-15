/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "access" and "faccessat".
 */

#include "libos_fs.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"

long libos_syscall_access(const char* file, mode_t mode) {
    return libos_syscall_faccessat(AT_FDCWD, file, mode);
}

static long do_faccessat(int dfd, const char* filename, mode_t mode, int flags) {
    if (mode & ~(R_OK | W_OK | X_OK))
        return -EINVAL;
    if (!is_user_string_readable(filename))
        return -EFAULT;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    int ret = 0;

    if (*filename != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    /* The kernel's faccessat(): the check uses the real ids (unless AT_EACCESS), and a real uid
     * other than 0 has no capabilities for it (LTP access01). The thread's filesystem ids and
     * effective set are swapped for the duration of the lookup and check. */
    struct libos_thread* cur = get_cur_thread();
    uid_t saved_fsuid = cur->fsuid;
    gid_t saved_fsgid = cur->fsgid;
    uint64_t saved_caps = cur->cap_effective;
    if (!(flags & AT_EACCESS)) {
        lock(&cur->lock);
        cur->fsuid = cur->uid;
        cur->fsgid = cur->gid;
        cur->cap_effective = cur->uid == 0 ? cur->cap_permitted : 0;
        unlock(&cur->lock);
    }

    lock(&g_dcache_lock);

    int lookup_flags = flags & AT_SYMLINK_NOFOLLOW ? 0 : LOOKUP_FOLLOW;
    ret = path_lookupat(dir, filename, lookup_flags, &dent);
    if (ret < 0)
        goto out;

    ret = check_permissions(dent, mode);

out:
    unlock(&g_dcache_lock);

    if (!(flags & AT_EACCESS)) {
        lock(&cur->lock);
        cur->fsuid = saved_fsuid;
        cur->fsgid = saved_fsgid;
        cur->cap_effective = saved_caps;
        unlock(&cur->lock);
    }

    if (dir)
        put_dentry(dir);
    if (dent) {
        put_dentry(dent);
    }
    return ret;
}

long libos_syscall_faccessat(int dfd, const char* filename, mode_t mode) {
    return do_faccessat(dfd, filename, mode, /*flags=*/0);
}

long libos_syscall_faccessat2(int dfd, const char* filename, mode_t mode, int flags) {
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW))
        return -EINVAL;

    return do_faccessat(dfd, filename, mode, flags);
}
