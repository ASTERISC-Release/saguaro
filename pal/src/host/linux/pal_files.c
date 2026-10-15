/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains operands to handle streams with URIs that start with "file:" or "dir:".
 */

#include <linux/fcntl.h> /* AT_FDCWD, AT_REMOVEDIR */
#include <time.h>

#include "api.h"
#include "linux_utils.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_flags_conv.h"
#include "pal_internal.h"
#include "pal_linux.h"
#include "pal_linux_error.h"
#include "path_utils.h"
#include "stat.h"

#define PAL_UTIME_NOW  ((1L << 30) - 1)
#define PAL_UTIME_OMIT ((1L << 30) - 2)

static int file_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                     pal_share_flags_t share, enum pal_create_mode create,
                     pal_stream_options_t options) {
    int ret;
    int fd = -1;
    PAL_HANDLE hdl = NULL;
    char* path = NULL;

    if (strcmp(type, URI_TYPE_FILE))
        return -PAL_ERROR_INVAL;

    assert(WITHIN_MASK(share,   PAL_SHARE_MASK));
    assert(WITHIN_MASK(options, PAL_OPTION_MASK));

    ret = DO_SYSCALL(openat, AT_FDCWD, uri, PAL_ACCESS_TO_LINUX_OPEN(access)  |
                                PAL_CREATE_TO_LINUX_OPEN(create)  |
                                PAL_OPTION_TO_LINUX_OPEN(options) |
                                O_CLOEXEC,
                     share);
    if (ret < 0)
        return unix_to_pal_error(ret);

    fd = ret;

    size_t uri_size = strlen(uri) + 1;
    hdl = calloc(1, HANDLE_SIZE(file));
    if (!hdl) {
        ret = -PAL_ERROR_NOMEM;
        goto fail;
    }

    init_handle_hdr(hdl, PAL_TYPE_FILE);
    hdl->flags |= PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;
    hdl->file.fd = fd;

    path = malloc(uri_size);
    if (!path) {
        ret = -PAL_ERROR_NOMEM;
        goto fail;
    }

    if (!get_norm_path(uri, path, &uri_size)) {
        ret = -PAL_ERROR_INVAL;
        goto fail;
    }

    hdl->file.realpath = path;

    struct stat st;
    ret = DO_SYSCALL(fstat, hdl->file.fd, &st);
    if (ret < 0) {
        ret = unix_to_pal_error(ret);
        goto fail;
    }

    hdl->file.seekable = !S_ISFIFO(st.st_mode);

    *handle = hdl;
    return 0;
fail:
    if (fd >= 0)
        DO_SYSCALL(close, fd);
    free(path);
    free(hdl);
    return ret;
}

static int64_t file_read(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buffer) {
    int64_t ret;
    if (handle->file.seekable) {
        ret = DO_SYSCALL(pread64, handle->file.fd, buffer, count, offset);
    } else {
        ret = DO_SYSCALL(read, handle->file.fd, buffer, count);
    }
    return ret < 0 ? unix_to_pal_error(ret) : ret;
}

static int64_t file_write(PAL_HANDLE handle, uint64_t offset, uint64_t count, const void* buffer) {
    int64_t ret;
    if (handle->file.seekable) {
        ret = DO_SYSCALL(pwrite64, handle->file.fd, buffer, count, offset);
    } else {
        ret = DO_SYSCALL(write, handle->file.fd, buffer, count);
    }
    return ret < 0 ? unix_to_pal_error(ret) : ret;
}

static void file_destroy(PAL_HANDLE handle) {
    assert(handle->hdr.type == PAL_TYPE_FILE);

    int ret = DO_SYSCALL(close, handle->file.fd);
    if (ret < 0) {
        log_error("closing file host fd %d failed: %s", handle->file.fd, unix_strerror(ret));
        /* We cannot do anything about it anyway... */
    }

    free(handle->file.realpath);
    free(handle);
}

static int file_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    if (delete_mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;

    DO_SYSCALL(unlinkat, AT_FDCWD, handle->file.realpath, 0);
    return 0;
}

static int file_map(PAL_HANDLE handle, void* addr, pal_prot_flags_t prot, uint64_t offset,
                    uint64_t size) {
    int flags = PAL_MEM_FLAGS_TO_LINUX(prot) | (addr ? MAP_FIXED_NOREPLACE : 0);
    int linux_prot = PAL_PROT_TO_LINUX(prot);

    addr = (void*)DO_SYSCALL(mmap, addr, size, linux_prot, flags, handle->file.fd, offset);
    if (IS_PTR_ERR(addr))
        return unix_to_pal_error(PTR_TO_ERR(addr));

    return 0;
}

static int file_setlength(PAL_HANDLE handle, uint64_t length) {
    int ret = DO_SYSCALL(ftruncate, handle->file.fd, length);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int file_flush(PAL_HANDLE handle) {
    int ret = DO_SYSCALL(fsync, handle->file.fd);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int file_attrquery(const char* type, const char* uri, PAL_STREAM_ATTR* attr) {
    if (strcmp(type, URI_TYPE_FILE) && strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;

    struct stat stat_buf;
    int ret = DO_SYSCALL(newfstatat, AT_FDCWD, uri, &stat_buf, 0);
    if (ret < 0)
        return unix_to_pal_error(ret);

    file_attrcopy(attr, &stat_buf);
    return 0;
}

static int file_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    struct stat stat_buf;
    int ret = DO_SYSCALL(fstat, handle->file.fd, &stat_buf);
    if (ret < 0)
        return unix_to_pal_error(ret);

    file_attrcopy(attr, &stat_buf);
    return 0;
}

static int file_attrsetbyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    if (attr->set_atime || attr->set_mtime) {
        struct timespec times[2] = {
            attr->set_atime ? (struct timespec){.tv_sec = attr->atime.tv_sec,
                                                 .tv_nsec = attr->atime.tv_nsec}
                            : (struct timespec){.tv_nsec = PAL_UTIME_OMIT},
            attr->set_mtime ? (struct timespec){.tv_sec = attr->mtime.tv_sec,
                                                 .tv_nsec = attr->mtime.tv_nsec}
                            : (struct timespec){.tv_nsec = PAL_UTIME_OMIT},
        };
        if (attr->atime_now)
            times[0].tv_nsec = PAL_UTIME_NOW;
        if (attr->mtime_now)
            times[1].tv_nsec = PAL_UTIME_NOW;
        int ret = DO_SYSCALL(utimensat, handle->file.fd, NULL, times, 0);
        return ret < 0 ? unix_to_pal_error(ret) : 0;
    }
    int ret = DO_SYSCALL(fchmod, handle->file.fd, attr->share_flags);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int file_rename(PAL_HANDLE handle, const char* type, const char* uri) {
    if (strcmp(type, URI_TYPE_FILE))
        return -PAL_ERROR_INVAL;

    char* tmp = strdup(uri);
    if (!tmp)
        return -PAL_ERROR_NOMEM;

#ifdef __NR_renameat
    int ret = DO_SYSCALL(renameat, AT_FDCWD, handle->file.realpath, AT_FDCWD, uri);
#else
    /* riscv64's asm/unistd.h (plain asm-generic, without the kernel's
     * __ARCH_WANT_RENAMEAT that x86_64/aarch64 opt into) has no renameat at all; the
     * ABI-sanctioned replacement is renameat2 with flags == 0, which is by definition
     * exactly renameat (see the comment in <asm-generic/unistd.h>). */
    int ret = DO_SYSCALL(renameat2, AT_FDCWD, handle->file.realpath, AT_FDCWD, uri, 0);
#endif
    if (ret < 0) {
        free(tmp);
        return unix_to_pal_error(ret);
    }

    free(handle->file.realpath);
    handle->file.realpath = tmp;
    return 0;
}

static int file_symlink(const char* target, const char* uri) {
    int ret = DO_SYSCALL(symlinkat, target, AT_FDCWD, uri);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int file_link(const char* old_uri, const char* new_uri) {
    int ret = DO_SYSCALL(linkat, AT_FDCWD, old_uri, AT_FDCWD, new_uri, 0);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int file_unlink_path(const char* uri) {
    int ret = DO_SYSCALL(unlinkat, AT_FDCWD, uri, 0);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int file_exchange_path(const char* uri_a, const char* uri_b) {
    int ret = DO_SYSCALL(renameat2, AT_FDCWD, uri_a, AT_FDCWD, uri_b, /*RENAME_EXCHANGE=*/2);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int file_readlink_path(const char* uri, char* buf, size_t* inout_size) {
    int ret = DO_SYSCALL(readlinkat, AT_FDCWD, uri, buf, *inout_size);
    if (ret < 0)
        return unix_to_pal_error(ret);
    *inout_size = (size_t)ret;
    return 0;
}

/* The host errors of the xattr calls that the LibOS must tell apart (the generic mapping turns
 * them all into PAL_ERROR_DENIED): see the PalFileXattr() comment in pal.h. */
static int pal_xattr_error(int unix_ret) {
    switch (-unix_ret) {
        case ENODATA:
            return -PAL_ERROR_STREAMNOTEXIST;
        case EOPNOTSUPP:
            return -PAL_ERROR_NOTSUPPORT;
        case ERANGE:
        case E2BIG:
            return -PAL_ERROR_TOOLONG;
        default:
            return unix_to_pal_error(unix_ret);
    }
}

static int file_xattr(const char* uri, enum pal_xattr_op op, const char* name, void* value,
                      size_t* inout_size, int flags) {
    bool nofollow = flags & PAL_XATTR_NOFOLLOW;
    long ret;
    switch (op) {
        case PAL_XATTR_GET:
            ret = nofollow ? DO_SYSCALL(lgetxattr, uri, name, value, *inout_size)
                           : DO_SYSCALL(getxattr, uri, name, value, *inout_size);
            break;
        case PAL_XATTR_SET: {
            int set_flags = flags & (PAL_XATTR_CREATE | PAL_XATTR_REPLACE); /* = XATTR_* */
            ret = nofollow ? DO_SYSCALL(lsetxattr, uri, name, value, *inout_size, set_flags)
                           : DO_SYSCALL(setxattr, uri, name, value, *inout_size, set_flags);
            break;
        }
        case PAL_XATTR_LIST:
            ret = nofollow ? DO_SYSCALL(llistxattr, uri, value, *inout_size)
                           : DO_SYSCALL(listxattr, uri, value, *inout_size);
            break;
        case PAL_XATTR_REMOVE:
            ret = nofollow ? DO_SYSCALL(lremovexattr, uri, name)
                           : DO_SYSCALL(removexattr, uri, name);
            break;
        default:
            return -PAL_ERROR_INVAL;
    }
    if (ret < 0)
        return pal_xattr_error((int)ret);
    if (op == PAL_XATTR_GET || op == PAL_XATTR_LIST)
        *inout_size = (size_t)ret;
    return 0;
}

static int file_rename_path(const char* old_uri, const char* new_uri) {
#ifdef __NR_renameat
    int ret = DO_SYSCALL(renameat, AT_FDCWD, old_uri, AT_FDCWD, new_uri);
#else
    int ret = DO_SYSCALL(renameat2, AT_FDCWD, old_uri, AT_FDCWD, new_uri, 0);
#endif
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int dir_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                    pal_share_flags_t share, enum pal_create_mode create,
                    pal_stream_options_t options) {
    __UNUSED(access);
    assert(create != PAL_CREATE_IGNORED);

    if (strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;

    if (create == PAL_CREATE_TRY || create == PAL_CREATE_ALWAYS) {
        int ret = DO_SYSCALL(mkdirat, AT_FDCWD, uri, share);

        if (ret < 0) {
            if (ret == -EEXIST && create == PAL_CREATE_ALWAYS)
                return -PAL_ERROR_STREAMEXIST;
            if (ret != -EEXIST)
                return unix_to_pal_error(ret);
            assert(ret == -EEXIST && create == PAL_CREATE_TRY);
        }
    }

    int fd = DO_SYSCALL(openat, AT_FDCWD, uri,
                        O_DIRECTORY | PAL_OPTION_TO_LINUX_OPEN(options) | O_CLOEXEC, 0);
    if (fd < 0)
        return unix_to_pal_error(fd);

    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(dir));
    if (!hdl) {
        DO_SYSCALL(close, fd);
        return -PAL_ERROR_NOMEM;
    }

    init_handle_hdr(hdl, PAL_TYPE_DIR);

    hdl->flags |= PAL_HANDLE_FD_READABLE;
    hdl->dir.fd = fd;

    char* path = strdup(uri);
    if (!path) {
        DO_SYSCALL(close, hdl->dir.fd);
        free(hdl);
        return -PAL_ERROR_NOMEM;
    }

    hdl->dir.realpath    = path;
    hdl->dir.buf         = NULL;
    hdl->dir.ptr         = NULL;
    hdl->dir.end         = NULL;
    hdl->dir.endofstream = false;

    *handle = hdl;
    return 0;
}

#define DIRBUF_SIZE 1024

static int64_t dir_read(PAL_HANDLE handle, uint64_t offset, size_t count, void* _buf) {
    size_t bytes_written = 0;
    char* buf = (char*)_buf;

    if (offset) {
        return -PAL_ERROR_INVAL;
    }

    if (handle->dir.endofstream) {
        return 0;
    }

    while (1) {
        while ((char*)handle->dir.ptr < (char*)handle->dir.end) {
            struct linux_dirent64* dirent = (struct linux_dirent64*)handle->dir.ptr;

            if (is_dot_or_dotdot(dirent->d_name)) {
                goto skip;
            }

            bool is_dir = dirent->d_type == DT_DIR;
            size_t len = strlen(dirent->d_name);

            if (len + 1 + (is_dir ? 1 : 0) > count) {
                goto out;
            }

            memcpy(buf, dirent->d_name, len);
            if (is_dir) {
                buf[len++] = '/';
            }
            buf[len++] = '\0';

            buf += len;
            bytes_written += len;
            count -= len;
        skip:
            handle->dir.ptr = (char*)handle->dir.ptr + dirent->d_reclen;
        }

        if (!count) {
            /* No space left, returning */
            goto out;
        }

        if (!handle->dir.buf) {
            handle->dir.buf = malloc(DIRBUF_SIZE);
            if (!handle->dir.buf) {
                return -PAL_ERROR_NOMEM;
            }
        }

        int size = DO_SYSCALL(getdents64, handle->dir.fd, handle->dir.buf, DIRBUF_SIZE);
        if (size < 0) {
            /* If something was written just return that and pretend
             * no error was seen - it will be caught next time. */
            if (bytes_written) {
                return bytes_written;
            }
            return unix_to_pal_error(size);
        }

        if (!size) {
            handle->dir.endofstream = true;
            goto out;
        }

        handle->dir.ptr = handle->dir.buf;
        handle->dir.end = (char*)handle->dir.buf + size;
    }

out:
    return (int64_t)bytes_written;
}

static void dir_destroy(PAL_HANDLE handle) {
    assert(handle->hdr.type == PAL_TYPE_DIR);

    int ret = DO_SYSCALL(close, handle->dir.fd);
    if (ret < 0) {
        log_error("closing dir host fd %d failed: %s", handle->dir.fd, unix_strerror(ret));
        /* We cannot do anything about it anyway... */
    }

    free(handle->dir.buf);
    free(handle->dir.realpath);
    free(handle);
}

static int dir_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    if (delete_mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;

    int ret = DO_SYSCALL(unlinkat, AT_FDCWD, handle->dir.realpath, AT_REMOVEDIR);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int dir_rename(PAL_HANDLE handle, const char* type, const char* uri) {
    if (strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;

    char* tmp = strdup(uri);
    if (!tmp)
        return -PAL_ERROR_NOMEM;

#ifdef __NR_renameat
    int ret = DO_SYSCALL(renameat, AT_FDCWD, handle->dir.realpath, AT_FDCWD, uri);
#else
    /* riscv64's asm/unistd.h (plain asm-generic, without the kernel's
     * __ARCH_WANT_RENAMEAT that x86_64/aarch64 opt into) has no renameat at all; the
     * ABI-sanctioned replacement is renameat2 with flags == 0, which is by definition
     * exactly renameat (see the comment in <asm-generic/unistd.h>). */
    int ret = DO_SYSCALL(renameat2, AT_FDCWD, handle->dir.realpath, AT_FDCWD, uri, 0);
#endif
    if (ret < 0) {
        free(tmp);
        return unix_to_pal_error(ret);
    }

    free(handle->dir.realpath);
    handle->dir.realpath = tmp;
    return 0;
}

struct handle_ops g_file_ops = {
    .open           = &file_open,
    .read           = &file_read,
    .write          = &file_write,
    .destroy        = &file_destroy,
    .delete         = &file_delete,
    .map            = &file_map,
    .setlength      = &file_setlength,
    .flush          = &file_flush,
    .attrquery      = &file_attrquery,
    .attrquerybyhdl = &file_attrquerybyhdl,
    .attrsetbyhdl   = &file_attrsetbyhdl,
    .rename         = &file_rename,
    .symlink        = &file_symlink,
    .link           = &file_link,
    .unlink_path    = &file_unlink_path,
    .rename_path    = &file_rename_path,
    .exchange_path  = &file_exchange_path,
    .readlink_path  = &file_readlink_path,
    .xattr          = &file_xattr,
};

struct handle_ops g_dir_ops = {
    .open           = &dir_open,
    .read           = &dir_read,
    .destroy        = &dir_destroy,
    .delete         = &dir_delete,
    .attrquery      = &file_attrquery,
    .attrquerybyhdl = &file_attrquerybyhdl,
    .attrsetbyhdl   = &file_attrsetbyhdl,
    .rename         = &dir_rename,
};
