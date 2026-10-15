/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "dup", "dup2" and "dup3".
 */

#include "libos_handle.h"
#include "libos_graphics.h"

#include "libos_internal.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "linux_abi/errors.h"

long libos_syscall_dup(unsigned int fd) {
    long retval;
    struct libos_handle* hdl;
    struct libos_handle_map* handle_map = get_thread_handle_map(NULL);
    assert(handle_map);

    hdl = get_fd_handle(fd, NULL, handle_map);
    if (!hdl)
        return -EBADF;

#ifdef ENABLE_GRAPHICS
    /* Kept below for future testing/cleanup */
    #if 0
    if(fd < fs_offset && !get_fd_handle(fd, NULL, handle_map))
        fd += fs_offset; // Hacky workaround: shift a handle-less fd into the host fd range (+500); host agent only
    if(fd > fs_offset){
        log_debug("redirecting dup(%d) to host", fd);

        long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
        c->p1 = (uint64_t) fd;
        __sync_synchronize();
        c->req_bit = DUP;
        comm_sync_notify(c);
        struct libos_handle* hdl_dup = get_new_handle();
        hdl_dup->type = TYPE_HOST;
        set_new_fd_handle_by_fd(c->ret, hdl_dup, FD_CLOEXEC, NULL);
        retval = c->ret;
        libos_syscall_relinquish_libos_lock(_offset);
        return retval;

    }
    #endif

    if(hdl->type == TYPE_HOST || hdl->type == TYPE_HOST_IDENTITY)
    {
        log_debug("redirecting dup(%d) to host", fd);
        long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
        c->p1 = (uint64_t) fd;
        __sync_synchronize();
        if (hdl->type == TYPE_HOST_IDENTITY) {
            c->req_bit = DUP_IDENTITY;
        } else {
            c->req_bit = DUP;
        }
        uint64_t comm_ret = comm_sync_notify(c);
        libos_syscall_relinquish_libos_lock(_offset);

        /* Create a new handle and mark it the same as the old handle */
        struct libos_handle* hdl_dup = get_new_handle();
        hdl_dup->type = hdl->type; 
        int ret = set_new_fd_handle_by_fd(comm_ret, hdl_dup, 0, NULL);
        if (ret < 0) {
            log_always("Failed to set new fd handle for dup: %d", ret);
            put_handle(hdl_dup);
            return ret;
        }

        put_handle(hdl);
        put_handle(hdl_dup);
        return comm_ret;
    }
#endif

    // dup() always zeroes fd flags
    int vfd = set_new_fd_handle(hdl, /*fd_flags=*/0, handle_map);
    put_handle(hdl);

#ifdef ENABLE_GRAPHICS
    /* Graphics logic to avoid overflow into fd range. */
    if (vfd > GRAPHICS_HOST_FD_OFFSET) {
        log_always("error: dup(..) overflowed into host fd range. (ret: %d)\n", vfd);
        assert(false);
    }
#endif 

    return vfd == -ENOMEM ? -EMFILE : vfd;
}

long libos_syscall_dup2(unsigned int oldfd, unsigned int newfd) {
    struct libos_handle_map* handle_map = get_thread_handle_map(NULL);
    assert(handle_map);

    struct libos_handle* hdl = get_fd_handle(oldfd, NULL, handle_map);
    if (!hdl)
        return -EBADF;
    if (newfd >= fd_table_limit(hdl)) {
        put_handle(hdl);
        return -EBADF;
    }

    if (oldfd == newfd) {
        put_handle(hdl);
        return newfd;
    }

    struct libos_handle* new_hdl = detach_fd_handle(newfd, NULL, handle_map);

    if (new_hdl)
        put_handle(new_hdl);

    // dup2() always zeroes fd flags
    int vfd = set_new_fd_handle_by_fd(newfd, hdl, /*fd_flags=*/0, handle_map);
    put_handle(hdl);
    return vfd == -ENOMEM ? -EMFILE : vfd;
}

long libos_syscall_dup3(unsigned int oldfd, unsigned int newfd, int flags) {
    if ((flags & ~O_CLOEXEC) || oldfd == newfd)
        return -EINVAL;

    struct libos_handle_map* handle_map = get_thread_handle_map(NULL);
    assert(handle_map);

    struct libos_handle* hdl = get_fd_handle(oldfd, NULL, handle_map);
    if (!hdl)
        return -EBADF;
    if (newfd >= fd_table_limit(hdl)) {
        put_handle(hdl);
        return -EBADF;
    }

    struct libos_handle* new_hdl = detach_fd_handle(newfd, NULL, handle_map);

    if (new_hdl)
        put_handle(new_hdl);

    int fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    int vfd = set_new_fd_handle_by_fd(newfd, hdl, fd_flags, handle_map);
    put_handle(hdl);
    return vfd == -ENOMEM ? -EMFILE : vfd;
}
