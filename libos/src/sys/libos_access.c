/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "access" and "faccessat".
 */

#include "libos_fs.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_table.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"

long libos_syscall_access(const char* file, mode_t mode) {
    return libos_syscall_faccessat(AT_FDCWD, file, mode);
}

static long do_faccessat(int dfd, const char* filename, mode_t mode, int flags) {
    if (!is_user_string_readable(filename))
        return -EFAULT;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    int ret = 0;

    if (*filename != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    lock(&g_dcache_lock);

    int lookup_flags = flags & AT_SYMLINK_NOFOLLOW ? 0 : LOOKUP_FOLLOW;
    ret = path_lookupat(dir, filename, lookup_flags, &dent);
    if (ret < 0)
        goto out;

    ret = check_permissions(dent, mode);

out:
    unlock(&g_dcache_lock);

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

    /* Gramine exposes a single fixed identity, so AT_EACCESS and real-ID checks are equivalent. */
    return do_faccessat(dfd, filename, mode, flags);
}
