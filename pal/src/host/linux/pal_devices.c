/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */
/* Copyright (C) 2020 Intel Labs */

/*
 * Operations to handle devices.
 *
 * TODO: Some devices allow lseek() but typically with device-specific semantics. Gramine currently
 *       emulates lseek() completely in LibOS layer, thus seeking at PAL layer cannot be correctly
 *       implemented (without device-specific changes to LibOS layer).
 */

#include <asm/socket.h>
#include <linux/fcntl.h> /* AT_FDCWD */
#include <linux/un.h>
#include <stdalign.h>

#include "api.h"
#include "ioctls.h"
#include "linux_socket.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_flags_conv.h"
#include "pal_internal.h"
#include "pal_linux.h"
#include "path_utils.h"
#include "perm.h"

/* The realpath of a device handle that wraps a host fd without a path (see _PalHostFdWrap()); such
 * a handle must not be deleted by name. */
#define HOST_FD_REALPATH "(host fd)"

static int dev_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                    pal_share_flags_t share, enum pal_create_mode create,
                    pal_stream_options_t options) {
    int ret;
    char* normpath = NULL;

    assert(create != PAL_CREATE_IGNORED);

    assert(WITHIN_MASK(share,   PAL_SHARE_MASK));
    assert(WITHIN_MASK(options, PAL_OPTION_MASK));

    if (strcmp(type, URI_TYPE_DEV))
        return -PAL_ERROR_INVAL;

    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(dev));
    if (!hdl)
        return -PAL_ERROR_NOMEM;

    init_handle_hdr(hdl, PAL_TYPE_DEV);

    hdl->dev.nonblocking = !!(options & PAL_OPTION_NONBLOCK);

    ret = DO_SYSCALL(openat, AT_FDCWD, uri, PAL_ACCESS_TO_LINUX_OPEN(access)  |
                                PAL_CREATE_TO_LINUX_OPEN(create)  |
                                PAL_OPTION_TO_LINUX_OPEN(options) |
                                O_CLOEXEC,
                     share);
    if (ret < 0) {
        ret = unix_to_pal_error(ret);
        goto fail;
    }
    hdl->dev.fd = ret;

    size_t normpath_size = strlen(uri) + 1;
    normpath = malloc(normpath_size);
    if (!normpath) {
        ret = -PAL_ERROR_NOMEM;
        goto fail;
    }
    ret = get_norm_path(uri, normpath, &normpath_size);
    if (ret < 0) {
        log_warning("Could not normalize path (%s): %s", uri, pal_strerror(ret));
        ret = -PAL_ERROR_DENIED;
        goto fail;
    }
    hdl->dev.realpath = normpath;

    if (access == PAL_ACCESS_RDONLY) {
        hdl->flags |= PAL_HANDLE_FD_READABLE;
    } else if (access == PAL_ACCESS_WRONLY) {
        hdl->flags |= PAL_HANDLE_FD_WRITABLE;
    } else {
        assert(access == PAL_ACCESS_RDWR);
        hdl->flags |= PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;
    }

    *handle = hdl;
    return 0;
fail:
    free(hdl);
    free(normpath);
    return ret;
}

static int64_t dev_read(PAL_HANDLE handle, uint64_t offset, uint64_t size, void* buffer) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    if (offset)
        return -PAL_ERROR_INVAL;

    if (!(handle->flags & PAL_HANDLE_FD_READABLE))
        return -PAL_ERROR_DENIED;

    int64_t bytes = DO_SYSCALL(read, handle->dev.fd, buffer, size);
    return bytes < 0 ? unix_to_pal_error(bytes) : bytes;
}

static int64_t dev_write(PAL_HANDLE handle, uint64_t offset, uint64_t size, const void* buffer) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    if (offset)
        return -PAL_ERROR_INVAL;

    if (!(handle->flags & PAL_HANDLE_FD_WRITABLE))
        return -PAL_ERROR_DENIED;

    int64_t bytes = DO_SYSCALL(write, handle->dev.fd, buffer, size);
    return bytes < 0 ? unix_to_pal_error(bytes) : bytes;
}

static void dev_destroy(PAL_HANDLE handle) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    int ret = DO_SYSCALL(close, handle->dev.fd);
    if (ret < 0) {
        log_error("closing dev host fd %d failed: %s", handle->dev.fd, unix_strerror(ret));
        /* We cannot do anything about it anyway... */
    }

    free(handle->dev.realpath);
    free(handle);
}

static int dev_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    if (delete_mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;

    if (!strcmp(handle->dev.realpath, HOST_FD_REALPATH))
        return -PAL_ERROR_INVAL;

    int ret = DO_SYSCALL(unlinkat, AT_FDCWD, handle->dev.realpath, 0);
    return ret < 0 ? unix_to_pal_error(ret) : ret;
}

static int dev_setlength(PAL_HANDLE handle, uint64_t length) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    int ret = DO_SYSCALL(ftruncate, handle->dev.fd, length);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int dev_map(PAL_HANDLE handle, void* addr, pal_prot_flags_t prot, uint64_t offset,
                   uint64_t size) {
    assert(handle->hdr.type == PAL_TYPE_DEV);
    assert(IS_ALLOC_ALIGNED(offset) && IS_ALLOC_ALIGNED(size));

    uint64_t dummy;
    if (__builtin_add_overflow(offset, size, &dummy)) {
        return -PAL_ERROR_INVAL;
    }

    if (addr < g_pal_public_state.shared_address_start
            || (uintptr_t)addr + size > (uintptr_t)g_pal_public_state.shared_address_end) {
        log_warning("Could not map a device outside of the shared memory range at %p-%p", addr,
                    addr + size);
        return -PAL_ERROR_DENIED;
    }

    void* mapped_addr = (void*)DO_SYSCALL(mmap, addr, size, PAL_PROT_TO_LINUX(prot),
                                          MAP_SHARED | MAP_FIXED_NOREPLACE, handle->dev.fd, offset);
    if (IS_PTR_ERR(mapped_addr))
        return unix_to_pal_error(PTR_TO_ERR(mapped_addr));

    assert(mapped_addr == addr);
    return 0;
}

static int dev_flush(PAL_HANDLE handle) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    int ret = DO_SYSCALL(fsync, handle->dev.fd);
    return ret < 0 ? unix_to_pal_error(ret) : 0;
}

static int dev_attrquery(const char* type, const char* uri, PAL_STREAM_ATTR* attr) {
    __UNUSED(type);
    assert(strcmp(type, URI_TYPE_DEV) == 0);

    struct stat stat_buf;
    int ret = DO_SYSCALL(newfstatat, AT_FDCWD, uri, &stat_buf, 0);
    if (ret < 0)
        return unix_to_pal_error(ret);

    attr->share_flags  = stat_buf.st_mode & PAL_SHARE_MASK;
    attr->pending_size = stat_buf.st_size;
    attr->handle_type = PAL_TYPE_DEV;
    attr->nonblocking = false;
    attr->dev.rdev_major = host_dev_major(stat_buf.st_rdev);
    attr->dev.rdev_minor = host_dev_minor(stat_buf.st_rdev);
    attr->dev.host_mode = stat_buf.st_mode;
    return 0;
}

static int dev_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    struct stat stat_buf;
    int ret = DO_SYSCALL(fstat, handle->dev.fd, &stat_buf);
    if (ret < 0)
        return unix_to_pal_error(ret);

    attr->share_flags  = stat_buf.st_mode & PAL_SHARE_MASK;
    attr->pending_size = stat_buf.st_size;
    attr->handle_type  = PAL_TYPE_DEV;
    attr->nonblocking  = handle->dev.nonblocking;
    attr->dev.rdev_major = host_dev_major(stat_buf.st_rdev);
    attr->dev.rdev_minor = host_dev_minor(stat_buf.st_rdev);
    attr->dev.host_mode = stat_buf.st_mode;
    return 0;
}

static int dev_attrsetbyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    assert(handle->hdr.type == PAL_TYPE_DEV);

    if (attr->nonblocking == handle->dev.nonblocking)
        return 0;
    int flags = DO_SYSCALL(fcntl, handle->dev.fd, F_GETFL);
    if (flags < 0)
        return unix_to_pal_error(flags);
    flags = attr->nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    int ret = DO_SYSCALL(fcntl, handle->dev.fd, F_SETFL, flags);
    if (ret < 0)
        return unix_to_pal_error(ret);
    handle->dev.nonblocking = attr->nonblocking;
    return 0;
}

struct handle_ops g_dev_ops = {
    .open           = &dev_open,
    .read           = &dev_read,
    .write          = &dev_write,
    .destroy        = &dev_destroy,
    .delete         = &dev_delete,
    .map            = &dev_map,
    .setlength      = &dev_setlength,
    .flush          = &dev_flush,
    .attrquery      = &dev_attrquery,
    .attrquerybyhdl = &dev_attrquerybyhdl,
    .attrsetbyhdl   = &dev_attrsetbyhdl,
};

int _PalHostFdWrap(int host_fd, PAL_HANDLE* out_handle) {
    int flags = DO_SYSCALL(fcntl, host_fd, F_GETFL);
    if (flags < 0)
        return unix_to_pal_error(flags);

    int ret = DO_SYSCALL(fcntl, host_fd, F_SETFD, FD_CLOEXEC);
    if (ret < 0)
        return unix_to_pal_error(ret);

    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(dev));
    if (!hdl)
        return -PAL_ERROR_NOMEM;
    init_handle_hdr(hdl, PAL_TYPE_DEV);
    hdl->dev.realpath = strdup(HOST_FD_REALPATH);
    if (!hdl->dev.realpath) {
        free(hdl);
        return -PAL_ERROR_NOMEM;
    }
    hdl->dev.fd = host_fd;
    hdl->dev.nonblocking = !!(flags & O_NONBLOCK);
    switch (flags & O_ACCMODE) {
        case O_RDONLY: hdl->flags |= PAL_HANDLE_FD_READABLE; break;
        case O_WRONLY: hdl->flags |= PAL_HANDLE_FD_WRITABLE; break;
        default:       hdl->flags |= PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE; break;
    }
    *out_handle = hdl;
    return 0;
}

int _PalHostFdGet(PAL_HANDLE handle, int* out_host_fd) {
    switch (handle->hdr.type) {
        case PAL_TYPE_DEV:
        case PAL_TYPE_FILE:
        case PAL_TYPE_SOCKET:
        case PAL_TYPE_EVENTFD:
            *out_host_fd = handle->generic.fd;
            return 0;
        default:
            return -PAL_ERROR_BADHANDLE;
    }
}

int _PalHostUnixConnect(const char* path, pal_stream_options_t options, PAL_HANDLE* out_handle) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(addr.sun_path))
        return -PAL_ERROR_INVAL;
    memcpy(addr.sun_path, path, len + 1);

    int fd = DO_SYSCALL(socket, AF_UNIX,
                        SOCK_STREAM | SOCK_CLOEXEC |
                            ((options & PAL_OPTION_NONBLOCK) ? SOCK_NONBLOCK : 0),
                        0);
    if (fd < 0)
        return unix_to_pal_error(fd);

    int ret = DO_SYSCALL(connect, fd, &addr, sizeof(addr));
    if (ret < 0 && ret != -EINPROGRESS) {
        DO_SYSCALL(close, fd);
        return unix_to_pal_error(ret);
    }

    ret = _PalHostFdWrap(fd, out_handle);
    if (ret < 0)
        DO_SYSCALL(close, fd);
    return ret;
}

/* At most this many fds go with one message; X clients pass a handful (dma-bufs, fences). */
#define HOST_MSG_MAX_FDS 64

int _PalHostSendmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, const int* host_fds,
                    size_t host_fds_cnt, bool nonblocking, size_t* out_size) {
    if (handle->hdr.type != PAL_TYPE_DEV)
        return -PAL_ERROR_BADHANDLE;
    if (host_fds_cnt > HOST_MSG_MAX_FDS)
        return -PAL_ERROR_INVAL;

    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = iov_len};
    alignas(struct cmsghdr) char control[CMSG_SPACE(HOST_MSG_MAX_FDS * sizeof(int))];
    if (host_fds_cnt) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(host_fds_cnt * sizeof(int));
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(host_fds_cnt * sizeof(int));
        memcpy(CMSG_DATA(cmsg), host_fds, host_fds_cnt * sizeof(int));
    }

    long ret = DO_SYSCALL(sendmsg, handle->dev.fd, &msg,
                          MSG_NOSIGNAL | (nonblocking ? MSG_DONTWAIT : 0));
    if (ret < 0)
        return unix_to_pal_error(ret);
    *out_size = (size_t)ret;
    return 0;
}

int _PalHostRecvmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, int* host_fds,
                    size_t* host_fds_cnt, bool nonblocking, size_t* out_size) {
    if (handle->hdr.type != PAL_TYPE_DEV)
        return -PAL_ERROR_BADHANDLE;

    size_t cap = MIN(*host_fds_cnt, (size_t)HOST_MSG_MAX_FDS);
    alignas(struct cmsghdr) char control[CMSG_SPACE(HOST_MSG_MAX_FDS * sizeof(int))];
    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = iov_len,
        .msg_control = cap ? control : NULL,
        .msg_controllen = cap ? CMSG_SPACE(cap * sizeof(int)) : 0,
    };

    long ret = DO_SYSCALL(recvmsg, handle->dev.fd, &msg,
                          MSG_CMSG_CLOEXEC | (nonblocking ? MSG_DONTWAIT : 0));
    if (ret < 0)
        return unix_to_pal_error(ret);

    /* one SCM_RIGHTS message at most: that is all a receiver gets from one recvmsg of a stream */
    size_t cnt = 0;
    struct cmsghdr* cmsg = cap ? CMSG_FIRSTHDR(&msg) : NULL;
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(0)) {
        size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n; i++) {
            int fd;
            memcpy(&fd, (char*)CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            if (cnt < cap)
                host_fds[cnt++] = fd;
            else
                DO_SYSCALL(close, fd);
        }
    }
    *host_fds_cnt = cnt;
    *out_size = (size_t)ret;
    return 0;
}

static bool memcpy_to_host(void* host_ptr, const void* ptr, size_t size) {
    memcpy(host_ptr, ptr, size);
    return true;
}

static bool memcpy_to_gramine(void* ptr, size_t max_size, const void* host_ptr, size_t host_size) {
    if (host_size > max_size)
        return false;
    memcpy(ptr, host_ptr, host_size);
    return true;
}

int _PalDeviceIoControl(PAL_HANDLE handle, uint32_t cmd, unsigned long arg, int* out_ret) {
    int ret;

    int fd;
    if (handle->hdr.type == PAL_TYPE_DEV)
        fd = handle->dev.fd;
    else if (handle->hdr.type == PAL_TYPE_SOCKET)
        fd = handle->sock.fd;
    else
        return -PAL_ERROR_INVAL;

    if (g_pal_linux_state.raw_device_ioctl) {
        /* `sys.insecure__allow_raw_device_ioctl`: the application shares the address space with
         * the host kernel here, so its argument pointer is passed unchanged, with no description
         * of the argument and no copy. Pointers inside the argument reach the host kernel as they
         * are, and fds in it are host fds (the LibOS translates the ones it knows about). */
        static bool warned = false;
        if (!__atomic_exchange_n(&warned, true, __ATOMIC_RELAXED)) {
            log_warning("Device ioctls are passed to the host unchecked "
                        "(sys.insecure__allow_raw_device_ioctl = true); "
                        "this is insecure and must not be used in production.");
        }
        *out_ret = DO_SYSCALL(ioctl, fd, cmd, arg);
        return 0;
    }

    /* find this IOCTL request in the manifest */
    toml_table_t* manifest_sys = toml_table_in(g_pal_public_state.manifest_root, "sys");
    if (!manifest_sys)
        return -PAL_ERROR_NOTIMPLEMENTED;

    toml_array_t* toml_ioctl_struct = NULL;
    ret = ioctls_get_allowed_ioctl_struct(manifest_sys, cmd, &toml_ioctl_struct);
    if (ret < 0)
        return ret;

    if (!toml_ioctl_struct) {
        /* special case of "no struct needed for IOCTL" -> base-type or ignored IOCTL argument */
        *out_ret = DO_SYSCALL(ioctl, fd, cmd, arg);
        return 0;
    }

    void* host_addr = NULL;
    size_t host_size = 0;

    size_t mem_regions_cnt = MAX_MEM_REGIONS;
    size_t sub_regions_cnt = MAX_SUB_REGIONS;
    struct mem_region* mem_regions = calloc(mem_regions_cnt, sizeof(*mem_regions));
    struct sub_region* sub_regions = calloc(sub_regions_cnt, sizeof(*sub_regions));
    if (!mem_regions || !sub_regions) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }

    /* deep-copy the IOCTL argument's input data to `host_addr` region, execute the IOCTL syscall, and
     * deep-copy the IOCTL argument's output data back into Gramine memory (this is redundant in
     * Linux PAL but we do it for uniformity with other PALs, notably Linux-SGX) */
    ret = ioctls_collect_sub_regions(manifest_sys, toml_ioctl_struct, (void*)arg, mem_regions,
                                     &mem_regions_cnt, sub_regions, &sub_regions_cnt);
    if (ret < 0) {
        log_error("IOCTL: failed to parse ioctl struct (request code = 0x%x)", cmd);
        goto out;
    }

    for (size_t i = 0; i < sub_regions_cnt; i++) {
        /* overapproximation since alignment doesn't necessarily increase sub-region's size */
        host_size += sub_regions[i].size + sub_regions[i].alignment - 1;
    }

    host_addr = calloc(1, ALLOC_ALIGN_UP(host_size));
    if (!host_addr) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }

    ret = ioctls_copy_sub_regions_to_host(sub_regions, sub_regions_cnt, host_addr, memcpy_to_host);
    if (ret < 0)
        goto out;

    /* note that if the host returned a negative value (typically means an error, but not always
     * since this is completely device-specific), then we still return success and forward the value
     * as-is to the LibOS and ultimately to the app */
    int ioctl_ret = DO_SYSCALL(ioctl, fd, cmd, (unsigned long)host_addr);

    ret = ioctls_copy_sub_regions_to_gramine(sub_regions, sub_regions_cnt, memcpy_to_gramine);
    if (ret < 0)
        goto out;

    *out_ret = ioctl_ret;
    ret = 0;
out:
    free(host_addr);
    free(mem_regions);
    free(sub_regions);
    return ret;
}
