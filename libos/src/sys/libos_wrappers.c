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

    ret = handle_append_position(hdl, &hdl->pos);
    if (ret < 0)
        goto out;

    ssize_t bytes = 0;
    for (size_t i = 0; i < vlen; i++) {
        if (!vec[i].iov_base)
            continue;

        size_t len = vec[i].iov_len;
        ret = handle_fsize_limit(hdl, hdl->pos, &len);
        if (ret < 0) {
            ret = bytes ?: ret;
            goto out;
        }
        ssize_t b_vec = hdl->fs->fs_ops->write(hdl, vec[i].iov_base, len, &hdl->pos);
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
    if (write && (ret = handle_append_position(hdl, &offset)) < 0)
        goto out;
    if (write && (ret = libos_memfd_check_write(hdl, offset, total_len)) < 0)
        goto out;
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

/* The kernel's preadv2/pwritev2 take (fd, vec, vlen, pos_l, pos_h, flags) on every architecture
 * (fs/read_write.c); on a 64-bit kernel pos_from_hilo() makes the offset pos_l alone, so pos_h
 * (the high half that libc also passes) is ignored and the flags are the sixth argument. */
long libos_syscall_preadv2(unsigned long fd, struct iovec* vec, unsigned long vlen,
                           unsigned long pos_l, unsigned long pos_h, int flags) {
    __UNUSED(pos_h);
    return do_preadv_pwritev(fd, vec, vlen, (loff_t)pos_l, false, flags, true);
}

long libos_syscall_pwritev2(unsigned long fd, struct iovec* vec, unsigned long vlen,
                            unsigned long pos_l, unsigned long pos_h, int flags) {
    __UNUSED(pos_h);
    return do_preadv_pwritev(fd, vec, vlen, (loff_t)pos_l, true, flags, true);
}
#endif

#define LIBOS_SPLICE_F_MOVE     1U
#define LIBOS_SPLICE_F_NONBLOCK 2U
#define LIBOS_SPLICE_F_MORE     4U
#define LIBOS_SPLICE_F_GIFT     8U
#define LIBOS_SPLICE_FLAGS \
    (LIBOS_SPLICE_F_MOVE | LIBOS_SPLICE_F_NONBLOCK | LIBOS_SPLICE_F_MORE | LIBOS_SPLICE_F_GIFT)
#define LIBOS_COPY_CHUNK (64U * 1024U)

long libos_syscall_copy_file_range(int fd_in, loff_t* off_in, int fd_out, loff_t* off_out,
                                   size_t len, unsigned int flags) {
    if (flags)
        return -EINVAL;
    if (off_in && (!is_user_memory_readable(off_in, sizeof(*off_in)) ||
                   !is_user_memory_writable(off_in, sizeof(*off_in))))
        return -EFAULT;
    if (off_out && (!is_user_memory_readable(off_out, sizeof(*off_out)) ||
                    !is_user_memory_writable(off_out, sizeof(*off_out))))
        return -EFAULT;

    struct libos_handle* in_hdl = get_fd_handle(fd_in, NULL, NULL);
    if (!in_hdl)
        return -EBADF;
    struct libos_handle* out_hdl = get_fd_handle(fd_out, NULL, NULL);
    if (!out_hdl) {
        put_handle(in_hdl);
        return -EBADF;
    }

    long ret = 0;
    if (in_hdl->type == TYPE_PIPE || out_hdl->type == TYPE_PIPE) {
        ret = -EINVAL;
        goto out;
    }
    if (!(in_hdl->acc_mode & MAY_READ) || !(out_hdl->acc_mode & MAY_WRITE)) {
        ret = -EBADF;
        goto out;
    }
    loff_t out_offset = off_out ? *off_out : out_hdl->pos;
    if ((ret = libos_memfd_check_write(out_hdl, out_offset, len)) < 0) {
        goto out;
    }
    if (!in_hdl->fs || !in_hdl->fs->fs_ops || !in_hdl->fs->fs_ops->read ||
        !out_hdl->fs || !out_hdl->fs->fs_ops || !out_hdl->fs->fs_ops->write) {
        ret = -EINVAL;
        goto out;
    }
    if (!in_hdl->fs->fs_ops->seek || !out_hdl->fs->fs_ops->seek) {
        ret = -ESPIPE;
        goto out;
    }
    if (in_hdl->inode && out_hdl->inode && in_hdl->inode->mount != out_hdl->inode->mount) {
        ret = -EXDEV;
        goto out;
    }

    file_off_t in_pos = in_hdl->pos;
    file_off_t out_pos = out_hdl->pos;
    if (off_in)
        memcpy(&in_pos, off_in, sizeof(in_pos));
    if (off_out)
        memcpy(&out_pos, off_out, sizeof(out_pos));
    if (in_pos < 0 || out_pos < 0) {
        ret = -EINVAL;
        goto out;
    }
    if (len > 0x7ffff000)
        len = 0x7ffff000;

    char* buffer = malloc(MIN(len, LIBOS_COPY_CHUNK));
    if (!buffer && len) {
        ret = -ENOMEM;
        goto out;
    }
    size_t copied = 0;
    while (copied < len) {
        size_t chunk = MIN(len - copied, LIBOS_COPY_CHUNK);
        ssize_t bytes = in_hdl->fs->fs_ops->read(in_hdl, buffer, chunk, &in_pos);
        if (bytes < 0) {
            ret = copied ? (long)copied : bytes;
            break;
        }
        if (!bytes)
            break;
        size_t written = 0;
        while (written < (size_t)bytes) {
            ssize_t out_bytes = out_hdl->fs->fs_ops->write(out_hdl, buffer + written,
                                                           (size_t)bytes - written, &out_pos);
            if (out_bytes < 0) {
                ret = copied + written ? (long)(copied + written) : out_bytes;
                goto copy_done;
            }
            if (!out_bytes)
                goto copy_done;
            written += out_bytes;
        }
        copied += written;
        if ((size_t)bytes < chunk)
            break;
    }
    if (ret == 0)
        ret = copied;
copy_done:
    free(buffer);
    if (off_in && copied)
        memcpy(off_in, &in_pos, sizeof(in_pos));
    else if (!off_in && copied)
        in_hdl->pos = in_pos;
    if (off_out && copied)
        memcpy(off_out, &out_pos, sizeof(out_pos));
    else if (!off_out && copied)
        out_hdl->pos = out_pos;
out:
    put_handle(out_hdl);
    put_handle(in_hdl);
    return ret == -EINTR ? -ERESTARTSYS : ret;
}

long libos_syscall_splice(int fd_in, loff_t* off_in, int fd_out, loff_t* off_out, size_t len,
                          unsigned int flags) {
    if (flags & ~LIBOS_SPLICE_FLAGS)
        return -EINVAL;
    if (off_in && !is_user_memory_readable(off_in, sizeof(*off_in)))
        return -EFAULT;
    if (off_out && !is_user_memory_readable(off_out, sizeof(*off_out)))
        return -EFAULT;
    if (off_in && off_out)
        return -EINVAL;
    struct libos_handle* in_hdl = get_fd_handle(fd_in, NULL, NULL);
    if (!in_hdl)
        return -EBADF;
    struct libos_handle* out_hdl = get_fd_handle(fd_out, NULL, NULL);
    if (!out_hdl) {
        put_handle(in_hdl);
        return -EBADF;
    }
    bool in_pipe = in_hdl->type == TYPE_PIPE;
    bool out_pipe = out_hdl->type == TYPE_PIPE;
    long ret = 0;
    if (!in_pipe && !out_pipe) {
        ret = -EINVAL;
        goto out;
    }
    if (!in_hdl->fs || !in_hdl->fs->fs_ops || !out_hdl->fs || !out_hdl->fs->fs_ops) {
        ret = -EINVAL;
        goto out;
    }
    if ((!in_pipe && off_in && !in_hdl->fs->fs_ops->seek) ||
        (!out_pipe && off_out && !out_hdl->fs->fs_ops->seek)) {
        ret = -ESPIPE;
        goto out;
    }
    if ((in_pipe && off_in) || (out_pipe && off_out)) {
        ret = -ESPIPE;
        goto out;
    }
    if (out_hdl->flags & O_APPEND) {
        ret = -EINVAL;
        goto out;
    }
    if (!(in_hdl->acc_mode & MAY_READ) || !(out_hdl->acc_mode & MAY_WRITE)) {
        ret = -EBADF;
        goto out;
    }
    loff_t out_offset = off_out ? *off_out : out_hdl->pos;
    if ((ret = libos_memfd_check_write(out_hdl, out_offset, len)) < 0) {
        goto out;
    }
    if (!in_hdl->fs || !in_hdl->fs->fs_ops || !in_hdl->fs->fs_ops->read ||
        !out_hdl->fs || !out_hdl->fs->fs_ops || !out_hdl->fs->fs_ops->write) {
        ret = -EINVAL;
        goto out;
    }
    file_off_t in_pos = in_pipe ? 0 : in_hdl->pos;
    file_off_t out_pos = out_pipe ? 0 : out_hdl->pos;
    if (off_in)
        memcpy(&in_pos, off_in, sizeof(in_pos));
    if (off_out)
        memcpy(&out_pos, off_out, sizeof(out_pos));
    if (in_pos < 0 || out_pos < 0) {
        ret = -EINVAL;
        goto out;
    }
    char* buffer = malloc(MIN(len, LIBOS_COPY_CHUNK));
    if (!buffer && len) {
        ret = -ENOMEM;
        goto out;
    }
    size_t transferred = 0;
    while (transferred < len) {
        size_t chunk = MIN(len - transferred, LIBOS_COPY_CHUNK);
        ssize_t bytes = in_hdl->fs->fs_ops->read(in_hdl, buffer, chunk, &in_pos);
        if (bytes < 0) {
            ret = transferred ? (long)transferred : bytes;
            break;
        }
        if (!bytes)
            break;
        ssize_t written = out_hdl->fs->fs_ops->write(out_hdl, buffer, bytes, &out_pos);
        if (written < 0) {
            ret = transferred ? (long)transferred : written;
            break;
        }
        transferred += written;
        if (written < bytes || (size_t)bytes < chunk)
            break;
    }
    free(buffer);
    if (off_in && transferred)
        memcpy(off_in, &in_pos, sizeof(in_pos));
    else if (!in_pipe && transferred)
        in_hdl->pos = in_pos;
    if (off_out && transferred)
        memcpy(off_out, &out_pos, sizeof(out_pos));
    else if (!out_pipe && transferred)
        out_hdl->pos = out_pos;
    if (ret == 0)
        ret = transferred;
out:
    put_handle(out_hdl);
    put_handle(in_hdl);
    return ret == -EINTR ? -ERESTARTSYS : ret;
}

long libos_syscall_tee(int fd_in, int fd_out, size_t len, unsigned int flags) {
    if (flags & ~LIBOS_SPLICE_FLAGS)
        return -EINVAL;
    struct libos_handle* in_hdl = get_fd_handle(fd_in, NULL, NULL);
    if (!in_hdl)
        return -EBADF;
    struct libos_handle* out_hdl = get_fd_handle(fd_out, NULL, NULL);
    if (!out_hdl) {
        put_handle(in_hdl);
        return -EBADF;
    }
    long ret = -EINVAL;
    if (in_hdl->type == TYPE_PIPE && out_hdl->type == TYPE_PIPE &&
        memcmp(in_hdl->info.pipe.name, out_hdl->info.pipe.name, PIPE_URI_SIZE)) {
        struct libos_handle* peer = get_fd_handle(in_hdl->info.pipe.peer_fd, NULL, NULL);
        if (!peer || peer->type != TYPE_PIPE ||
            memcmp(peer->info.pipe.name, in_hdl->info.pipe.name, PIPE_URI_SIZE)) {
            ret = -EOPNOTSUPP;
        } else if (!(in_hdl->acc_mode & MAY_READ) || !(out_hdl->acc_mode & MAY_WRITE) ||
                   !(peer->acc_mode & MAY_WRITE)) {
            ret = -EBADF;
        } else if (len > 0) {
            PAL_STREAM_ATTR attr;
            int pal_ret = PalStreamAttributesQueryByHandle(in_hdl->pal_handle, &attr);
            if (pal_ret < 0) {
                ret = pal_to_unix_errno(pal_ret);
            } else if (!attr.pending_size) {
                ret = -EAGAIN;
            } else if (attr.pending_size > 1024 * 1024) {
                ret = -EOPNOTSUPP;
            } else {
                size_t pending = attr.pending_size;
                char* buffer = malloc(pending);
                if (!buffer) {
                    ret = -ENOMEM;
                } else {
                    file_off_t pos = 0;
                    size_t read_count = 0;
                    while (read_count < pending) {
                        ssize_t n = in_hdl->fs->fs_ops->read(in_hdl, buffer + read_count,
                                                             pending - read_count, &pos);
                        if (n <= 0) {
                            ret = n < 0 ? n : -EIO;
                            break;
                        }
                        read_count += n;
                    }
                    size_t duplicate = MIN(len, read_count);
                    size_t written = 0;
                    bool dest_ok = true;
                    while (written < duplicate) {
                        ssize_t n = out_hdl->fs->fs_ops->write(out_hdl, buffer + written,
                                                               duplicate - written, &pos);
                        if (n <= 0) {
                            ret = n < 0 ? n : -EIO;
                            dest_ok = false;
                            break;
                        }
                        written += n;
                    }
                    pos = 0;
                    written = 0;
                    while (written < read_count) {
                        ssize_t n = peer->fs->fs_ops->write(peer, buffer + written,
                                                            read_count - written, &pos);
                        if (n <= 0) {
                            ret = n < 0 ? n : -EIO;
                            break;
                        }
                        written += n;
                    }
                    if (written == read_count && dest_ok)
                        ret = duplicate;
                    free(buffer);
                }
            }
        } else {
            ret = 0;
        }
        if (peer)
            put_handle(peer);
    }
    put_handle(out_hdl);
    put_handle(in_hdl);
    return ret;
}

long libos_syscall_vmsplice(int fd, const struct iovec* iov, unsigned long nr_segs,
                            unsigned int flags) {
    if (flags & ~LIBOS_SPLICE_FLAGS)
        return -EINVAL;
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;
    bool is_pipe = hdl->type == TYPE_PIPE;
    bool can_read = hdl->acc_mode & MAY_READ;
    bool can_write = hdl->acc_mode & MAY_WRITE;
    put_handle(hdl);
    if (!is_pipe)
        return -EBADF;
    if (nr_segs > 1024)
        return -EINVAL;
    if (!can_read && !can_write)
        return -EBADF;
    if ((flags & LIBOS_SPLICE_F_GIFT) && !can_write)
        return -EINVAL;
    /* PAL pipe writes copy the supplied bytes, so GIFT does not transfer page ownership. */
    if (can_write)
        return libos_syscall_writev(fd, (struct iovec*)iov, nr_segs);
    return libos_syscall_readv(fd, (struct iovec*)iov, nr_segs);
}
