/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "readv" and "writev".
 */

#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "linux_abi/errors.h"

/* TODO: `readv` and `writev` syscalls below are not correctly atomic if the implementation does not
 * provide `.readv` and `.writev` callbacks and does not use file position (`hdl->pos`). This most
 * notably affects pipes. */

long libos_syscall_readv(unsigned long fd, struct iovec* vec, unsigned long vlen) {
    size_t arr_size;
    if (__builtin_mul_overflow(sizeof(*vec), vlen, &arr_size))
        return -EINVAL;
    if (!is_user_memory_readable(vec, arr_size))
        return -EINVAL;

    for (size_t i = 0; i < vlen; i++) {
        if (vec[i].iov_base) {
            if (!access_ok(vec[i].iov_base, vec[i].iov_len))
                return -EINVAL;
            if (!is_user_memory_writable(vec[i].iov_base, vec[i].iov_len))
                return -EFAULT;
        }
    }

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    maybe_lock_pos_handle(hdl);

    int ret = 0;

    if (hdl->is_dir) {
        ret = -EISDIR;
        goto out;
    }

    if (!(hdl->acc_mode & MAY_READ) || !hdl->fs || !hdl->fs->fs_ops) {
        ret = -EACCES;
        goto out;
    }

    if (hdl->fs->fs_ops->readv) {
        ret = hdl->fs->fs_ops->readv(hdl, vec, vlen, &hdl->pos);
        goto out;
    }

    if (!hdl->fs->fs_ops->read) {
        ret = -EACCES;
        goto out;
    }

    ssize_t bytes = 0;
    for (size_t i = 0; i < vlen; i++) {
        if (!vec[i].iov_base)
            continue;

        ssize_t b_vec = hdl->fs->fs_ops->read(hdl, vec[i].iov_base, vec[i].iov_len, &hdl->pos);
        if (b_vec < 0) {
            ret = bytes ?: b_vec;
            goto out;
        }

        bytes += b_vec;
    }

    ret = bytes;
out:
    maybe_unlock_pos_handle(hdl);
    put_handle(hdl);
    if (ret == -EINTR) {
        ret = -ERESTARTSYS;
    }
    return ret;
}

long libos_syscall_writev(unsigned long fd, struct iovec* vec, unsigned long vlen) {
    size_t arr_size;
    if (__builtin_mul_overflow(sizeof(*vec), vlen, &arr_size))
        return -EINVAL;
    if (!is_user_memory_readable(vec, arr_size))
        return -EINVAL;

    for (size_t i = 0; i < vlen; i++) {
        if (vec[i].iov_base) {
            if (!access_ok(vec[i].iov_base, vec[i].iov_len))
                return -EINVAL;
            if (!is_user_memory_readable(vec[i].iov_base, vec[i].iov_len))
                return -EFAULT;
        }
    }

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    maybe_lock_pos_handle(hdl);

    int ret = 0;

    if (hdl->is_dir) {
        ret = -EISDIR;
        goto out;
    }

    if (!(hdl->acc_mode & MAY_WRITE) || !hdl->fs || !hdl->fs->fs_ops) {
        ret = -EACCES;
        goto out;
    }

    if (hdl->fs->fs_ops->writev) {
        ret = hdl->fs->fs_ops->writev(hdl, vec, vlen, &hdl->pos);
        goto out;
    }

    if (!hdl->fs->fs_ops->write) {
        ret = -EACCES;
        goto out;
    }

    ssize_t bytes = 0;
    for (size_t i = 0; i < vlen; i++) {
        if (!vec[i].iov_base)
            continue;

        ssize_t b_vec = hdl->fs->fs_ops->write(hdl, vec[i].iov_base, vec[i].iov_len, &hdl->pos);
        if (b_vec < 0) {
            ret = bytes ?: b_vec;
            goto out;
        }

        bytes += b_vec;
    }

    ret = bytes;
out:
    maybe_unlock_pos_handle(hdl);
    put_handle(hdl);
    if (ret == -EINTR) {
        ret = -ERESTARTSYS;
    }
    return ret;
}

#define LIBOS_MAX_IOV 1024
#define LIBOS_MAX_RW_COUNT 0x7ffff000
#define LIBOS_RWF_HIPRI  0x00000001
#define LIBOS_RWF_DSYNC  0x00000002
#define LIBOS_RWF_SYNC   0x00000004
#define LIBOS_RWF_NOWAIT 0x00000008
#define LIBOS_RWF_APPEND 0x00000010

static long do_preadv_pwritev(unsigned long fd, struct iovec* vec, unsigned long vlen,
                              loff_t offset, bool write, int flags, bool allow_current) {
    const int supported_flags = LIBOS_RWF_HIPRI | LIBOS_RWF_DSYNC | LIBOS_RWF_SYNC |
                                LIBOS_RWF_NOWAIT | LIBOS_RWF_APPEND;
    if (flags & ~supported_flags)
        return -EOPNOTSUPP;
    if (offset < 0 && !(allow_current && offset == -1))
        return -EINVAL;
    if (vlen > LIBOS_MAX_IOV)
        return -EINVAL;

    size_t arr_size;
    if (__builtin_mul_overflow(sizeof(*vec), vlen, &arr_size))
        return -EINVAL;
    if (!is_user_memory_readable(vec, arr_size))
        return -EFAULT;

    size_t total_len = 0;
    for (size_t i = 0; i < vlen; i++) {
        if (vec[i].iov_len > SSIZE_MAX ||
            __builtin_add_overflow(total_len, vec[i].iov_len, &total_len) ||
            total_len > LIBOS_MAX_RW_COUNT)
            return -EINVAL;
    }
    if (offset >= 0 && (uint64_t)offset + total_len > INT64_MAX)
        return -EINVAL;

    for (size_t i = 0; i < vlen; i++) {
        if (!vec[i].iov_base && vec[i].iov_len)
            return -EFAULT;
        if (!access_ok(vec[i].iov_base, vec[i].iov_len))
            return -EFAULT;
        if (write) {
            if (!is_user_memory_readable(vec[i].iov_base, vec[i].iov_len))
                return -EFAULT;
        } else if (!is_user_memory_writable(vec[i].iov_base, vec[i].iov_len)) {
            return -EFAULT;
        }
    }

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    long ret = 0;
    bool current_offset = allow_current && offset == -1;
    bool position_locked = false;
    if (hdl->is_dir) {
        ret = -EISDIR;
        goto out;
    }
    if (!hdl->fs || !hdl->fs->fs_ops || !hdl->fs->fs_ops->seek) {
        ret = -ESPIPE;
        goto out;
    }
    if (!(hdl->acc_mode & (write ? MAY_WRITE : MAY_READ))) {
        ret = -EBADF;
        goto out;
    }
    if (current_offset) {
        maybe_lock_pos_handle(hdl);
        position_locked = true;
        offset = hdl->pos;
    }
    if (write && (flags & LIBOS_RWF_APPEND) && hdl->inode) {
        lock(&hdl->inode->lock);
        offset = hdl->inode->size;
        unlock(&hdl->inode->lock);
    }
    if (write ? !hdl->fs->fs_ops->write : !hdl->fs->fs_ops->read) {
        ret = -EACCES;
        goto out;
    }

    ssize_t total = 0;
    for (size_t i = 0; i < vlen; i++) {
        file_off_t pos = offset;
        ssize_t done = write ? hdl->fs->fs_ops->write(hdl, vec[i].iov_base, vec[i].iov_len, &pos)
                             : hdl->fs->fs_ops->read(hdl, vec[i].iov_base, vec[i].iov_len, &pos);
        if (done < 0) {
            ret = total ? total : done;
            goto out;
        }
        total += done;
        offset += done;
        if ((size_t)done < vec[i].iov_len)
            break;
    }
    ret = total;
    if (current_offset && ret > 0 && !(write && (flags & LIBOS_RWF_APPEND)))
        hdl->pos = offset;
    if (write && (flags & (LIBOS_RWF_DSYNC | LIBOS_RWF_SYNC)) && hdl->fs->fs_ops->flush) {
        int flush_ret = hdl->fs->fs_ops->flush(hdl);
        if (flush_ret < 0 && ret >= 0)
            ret = flush_ret;
    }
out:
    if (position_locked)
        maybe_unlock_pos_handle(hdl);
    put_handle(hdl);
    return ret == -EINTR ? -ERESTARTSYS : ret;
}

#ifdef __x86_64__
static loff_t x86_64_iov_offset(unsigned long pos_l, unsigned long pos_h) {
    loff_t offset = (loff_t)(((uint64_t)(uint32_t)pos_h << 32) | (uint32_t)pos_l);
    /* The x86_64 libc syscall wrapper zero-extends the low half for a 32-bit -1 offset. */
    if ((uint32_t)pos_l == UINT32_MAX && pos_h == 0)
        return -1;
    return offset;
}

long libos_syscall_preadv(unsigned long fd, struct iovec* vec, unsigned long vlen,
                          unsigned long pos_l, unsigned long pos_h) {
    return do_preadv_pwritev(fd, vec, vlen, x86_64_iov_offset(pos_l, pos_h), false, 0, false);
}

long libos_syscall_pwritev(unsigned long fd, struct iovec* vec, unsigned long vlen,
                           unsigned long pos_l, unsigned long pos_h) {
    return do_preadv_pwritev(fd, vec, vlen, x86_64_iov_offset(pos_l, pos_h), true, 0, false);
}

long libos_syscall_preadv2(unsigned long fd, struct iovec* vec, unsigned long vlen,
                           unsigned long pos_l, unsigned long pos_h, int flags) {
    return do_preadv_pwritev(fd, vec, vlen, x86_64_iov_offset(pos_l, pos_h), false, flags, true);
}

long libos_syscall_pwritev2(unsigned long fd, struct iovec* vec, unsigned long vlen,
                            unsigned long pos_l, unsigned long pos_h, int flags) {
    return do_preadv_pwritev(fd, vec, vlen, x86_64_iov_offset(pos_l, pos_h), true, flags, true);
}
#else
long libos_syscall_preadv(unsigned long fd, struct iovec* vec, unsigned long vlen, loff_t offset) {
    return do_preadv_pwritev(fd, vec, vlen, offset, false, 0, false);
}

long libos_syscall_pwritev(unsigned long fd, struct iovec* vec, unsigned long vlen, loff_t offset) {
    return do_preadv_pwritev(fd, vec, vlen, offset, true, 0, false);
}

long libos_syscall_preadv2(unsigned long fd, struct iovec* vec, unsigned long vlen, loff_t offset,
                           int flags) {
    return do_preadv_pwritev(fd, vec, vlen, offset, false, flags, true);
}

long libos_syscall_pwritev2(unsigned long fd, struct iovec* vec, unsigned long vlen, loff_t offset,
                            int flags) {
    return do_preadv_pwritev(fd, vec, vlen, offset, true, flags, true);
}
#endif
