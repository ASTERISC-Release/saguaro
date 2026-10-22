/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains operands to handle streams with URIs that start with "pipe:" or "pipe.srv:".
 */

#include <asm/fcntl.h>
#include <asm/ioctls.h>
#include <asm/poll.h>
#include <asm/stat.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/un.h>

#include <limits.h>
#include <stdalign.h>

#include "api.h"
#include "linux_socket.h"
#include "linux_utils.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_flags_conv.h"
#include "pal_internal.h"
#include "pal_linux.h"

/*!
 * \brief Create a listening abstract UNIX socket as preparation for connecting two ends of a pipe.
 *
 * \param[out] handle   PAL handle of type `pipesrv` with abstract UNIX socket opened for listening.
 * \param      name     String uniquely identifying the pipe.
 * \param      options  May contain PAL_OPTION_NONBLOCK.
 *
 * \returns 0 on success, negative PAL error code otherwise.
 *
 * An abstract UNIX socket with name "/gramine/<pipename>" is opened for listening. A corresponding
 * PAL handle with type `pipesrv` is created. This PAL handle typically serves only as an
 * intermediate step to connect two ends of the pipe (`pipecli` and `pipe`). As soon as the other
 * end of the pipe connects to this listening socket, a new accepted socket and the corresponding
 * PAL handle are created, and this `pipesrv` handle can be closed.
 */
static int pipe_listen(PAL_HANDLE* handle, const char* name, pal_stream_options_t options) {
    int ret;

    struct sockaddr_un addr;
    ret = get_gramine_unix_socket_addr(name, &addr);
    if (ret < 0)
        return -PAL_ERROR_DENIED;

    int nonblock = options & PAL_OPTION_NONBLOCK ? SOCK_NONBLOCK : 0;

    int fd = DO_SYSCALL(socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | nonblock, 0);
    if (fd < 0)
        return unix_to_pal_error(fd);

    ret = DO_SYSCALL(bind, fd, &addr, sizeof(addr.sun_path) - 1);
    if (ret < 0) {
        DO_SYSCALL(close, fd);
        return unix_to_pal_error(ret);
    }

    ret = DO_SYSCALL(listen, fd, 1);
    if (ret < 0) {
        DO_SYSCALL(close, fd);
        return unix_to_pal_error(ret);
    }

    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(pipe));
    if (!hdl) {
        DO_SYSCALL(close, fd);
        return -PAL_ERROR_NOMEM;
    }

    init_handle_hdr(hdl, PAL_TYPE_PIPESRV);
    hdl->flags |= PAL_HANDLE_FD_READABLE;  /* cannot write to a listening socket */
    hdl->pipe.fd            = fd;
    hdl->pipe.nonblocking   = !!(options & PAL_OPTION_NONBLOCK);

    *handle = hdl;
    return 0;
}

/*!
 * \brief Accept the other end of the pipe and create PAL handle for our end of the pipe.
 *
 * \param      handle   PAL handle of type `pipesrv` with abstract UNIX socket opened for listening.
 * \param[out] client   PAL handle of type `pipecli` connected to the other end of the pipe (`pipe`).
 * \param      options  flags to set on \p client handle.
 *
 * \returns 0 on success, negative PAL error code otherwise.
 *
 * Caller creates a `pipesrv` PAL handle with the underlying abstract UNIX socket opened for
 * listening, and then calls this function to wait for the other end of the pipe to connect.
 * When the connection request arrives, a new `pipecli` PAL handle is created with the
 * corresponding underlying socket and is returned in `client`. This `pipecli` PAL handle denotes
 * our end of the pipe. Typically, `pipesrv` handle is not needed after this and can be closed.
 */
static int pipe_waitforclient(PAL_HANDLE handle, PAL_HANDLE* client, pal_stream_options_t options) {
    if (handle->hdr.type != PAL_TYPE_PIPESRV)
        return -PAL_ERROR_NOTSERVER;

    static_assert(O_NONBLOCK == SOCK_NONBLOCK, "assumed below");
    int flags = PAL_OPTION_TO_LINUX_OPEN(options) | SOCK_CLOEXEC;
    int newfd = DO_SYSCALL(accept4, handle->pipe.fd, NULL, NULL, flags);
    if (newfd < 0)
        return unix_to_pal_error(newfd);

    PAL_HANDLE clnt = calloc(1, HANDLE_SIZE(pipe));
    if (!clnt) {
        DO_SYSCALL(close, newfd);
        return -PAL_ERROR_NOMEM;
    }

    init_handle_hdr(clnt, PAL_TYPE_PIPECLI);
    clnt->flags |= PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;
    clnt->pipe.fd            = newfd;
    clnt->pipe.nonblocking   = !!(flags & SOCK_NONBLOCK);

    *client = clnt;
    return 0;
}

/*!
 * \brief Connect to the other end of the pipe and create PAL handle for our end of the pipe.
 *
 * \param[out] handle   PAL handle of type `pipe` with abstract UNIX socket connected to another end.
 * \param      name     String uniquely identifying the pipe.
 * \param      options  May contain PAL_OPTION_NONBLOCK.
 *
 * \returns 0 on success, negative PAL error code otherwise.
 *
 * This function connects to the other end of the pipe, represented as an abstract UNIX socket
 * "/gramine/<pipename>" opened for listening. When the connection succeeds, a new `pipe` PAL handle
 * is created with the corresponding underlying socket and is returned in `handle`. The other end of
 * the pipe is typically of type `pipecli`.
 */
static int pipe_connect(PAL_HANDLE* handle, const char* name, pal_stream_options_t options) {
    int ret;

    struct sockaddr_un addr;
    ret = get_gramine_unix_socket_addr(name, &addr);
    if (ret < 0)
        return -PAL_ERROR_DENIED;

    int nonblock = options & PAL_OPTION_NONBLOCK ? SOCK_NONBLOCK : 0;

    int fd = DO_SYSCALL(socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | nonblock, 0);
    if (fd < 0)
        return -PAL_ERROR_DENIED;

    ret = DO_SYSCALL(connect, fd, &addr, sizeof(addr.sun_path) - 1);
    if (ret < 0) {
        DO_SYSCALL(close, fd);
        return unix_to_pal_error(ret);
    }

    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(pipe));
    if (!hdl) {
        DO_SYSCALL(close, fd);
        return -PAL_ERROR_NOMEM;
    }

    init_handle_hdr(hdl, PAL_TYPE_PIPE);
    hdl->flags |= PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;
    hdl->pipe.fd            = fd;
    hdl->pipe.nonblocking   = !!(options & PAL_OPTION_NONBLOCK);

    *handle = hdl;
    return 0;
}

/*!
 * \brief Create PAL handle of type `pipesrv` or `pipe` depending on `type` and `uri`.
 *
 * \param[out] handle   Created PAL handle of type `pipesrv` or `pipe`.
 * \param      type     Can be URI_TYPE_PIPE or URI_TYPE_PIPE_SRV.
 * \param      uri      Content is either NUL (for anonymous pipe) or a string with pipe name.
 * \param      access   Not used.
 * \param      share    Not used.
 * \param      create   Not used.
 * \param      options  May contain PAL_OPTION_NONBLOCK.
 *
 * \returns 0 on success, negative PAL error code otherwise.
 *
 * Depending on the combination of `type` and `uri`, the following PAL handles are created:
 *
 * - `type` is URI_TYPE_PIPE_SRV: create `pipesrv` handle (intermediate listening socket) with
 *                                the name created by `get_gramine_unix_socket_addr`. Caller is
 *                                expected to call pipe_waitforclient() afterwards.
 *
 * - `type` is URI_TYPE_PIPE: create `pipe` handle (connecting socket) with the name created by
 *                            `get_gramine_unix_socket_addr`.
 */
static int pipe_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                     pal_share_flags_t share, enum pal_create_mode create,
                     pal_stream_options_t options) {
    __UNUSED(access);
    __UNUSED(create);
    assert(create == PAL_CREATE_IGNORED);

    if (!WITHIN_MASK(share, PAL_SHARE_MASK) || !WITHIN_MASK(options, PAL_OPTION_MASK))
        return -PAL_ERROR_INVAL;

    if (!strcmp(type, URI_TYPE_PIPE_SRV))
        return pipe_listen(handle, uri, options);

    if (!strcmp(type, URI_TYPE_PIPE))
        return pipe_connect(handle, uri, options);

    return -PAL_ERROR_INVAL;
}

/*!
 * \brief Read from pipe.
 *
 * \param      handle  PAL handle of type `pipecli` or `pipe`.
 * \param      offset  Not used.
 * \param      len     Size of user-supplied buffer.
 * \param[out] buffer  User-supplied buffer to read data to.
 *
 * \returns Number of bytes read on success, negative PAL error code otherwise.
 */
static int64_t pipe_read(PAL_HANDLE handle, uint64_t offset, uint64_t len, void* buffer) {
    if (offset)
        return -PAL_ERROR_INVAL;

    if (handle->hdr.type != PAL_TYPE_PIPECLI && handle->hdr.type != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;

    ssize_t bytes = DO_SYSCALL(read, handle->pipe.fd, buffer, len);
    if (bytes < 0)
        return unix_to_pal_error(bytes);

    return bytes;
}

/*!
 * \brief Write to pipe.
 *
 * \param handle  PAL handle of type `pipecli` or `pipe`.
 * \param offset  Not used.
 * \param len     Size of user-supplied buffer.
 * \param buffer  User-supplied buffer to write data from.
 *
 * \returns Number of bytes written on success, negative PAL error code otherwise.
 */
static int64_t pipe_write(PAL_HANDLE handle, uint64_t offset, size_t len, const void* buffer) {
    if (offset)
        return -PAL_ERROR_INVAL;

    if (handle->hdr.type != PAL_TYPE_PIPECLI && handle->hdr.type != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;

    ssize_t bytes = DO_SYSCALL(write, handle->pipe.fd, buffer, len);
    if (bytes < 0)
        return unix_to_pal_error(bytes);

    return bytes;
}

/*
 * Handles attached to data (PalStreamWriteWithHandles): the pipe is a host Unix stream socket, so
 * the host carries the fds with the bytes (`SCM_RIGHTS`) and keeps the message boundaries. The
 * serialised PAL handles and the caller's blob travel in a memfd sent as the first fd, so that the
 * data stream itself stays untouched; a cargo handle without a host fd (a closed end) is sent as
 * its serialised form alone.
 */
#define PIPE_HANDLES_MAGIC 0x53444846u /* "FHDS" */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MSG_CTRUNC
#define MSG_CTRUNC 0x08
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif

struct pipe_handles_hdr {
    uint32_t magic;
    uint32_t cargo_count;
    uint32_t meta_size;
    uint32_t reserved;
};

struct pipe_cargo_hdr {
    uint32_t has_fd;
    uint32_t data_size;
};

static int64_t pipe_writehandles(PAL_HANDLE handle, const void* buffer, size_t count,
                                 PAL_HANDLE* cargo, size_t cargo_count, const void* meta,
                                 size_t meta_size) {
    if (handle->hdr.type != PAL_TYPE_PIPECLI && handle->hdr.type != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;
    if (cargo_count == 0)
        return pipe_write(handle, 0, count, buffer);
    if (cargo_count > PAL_STREAM_MAX_HANDLES || meta_size > UINT32_MAX)
        return -PAL_ERROR_INVAL;

    int ret;
    void* datas[PAL_STREAM_MAX_HANDLES];
    int sizes[PAL_STREAM_MAX_HANDLES];
    size_t blob_size = sizeof(struct pipe_handles_hdr) + meta_size;
    size_t serialized = 0;
    int fds[1 + PAL_STREAM_MAX_HANDLES];
    size_t fds_count = 1;
    for (; serialized < cargo_count; serialized++) {
        if (cargo[serialized]->hdr.type == PAL_TYPE_PROCESS) {
            ret = -PAL_ERROR_NOTIMPLEMENTED;
            goto out_free;
        }
        ret = handle_serialize(cargo[serialized], &datas[serialized]);
        if (ret < 0)
            goto out_free;
        sizes[serialized] = ret;
        blob_size += sizeof(struct pipe_cargo_hdr) + ret;
        if (cargo[serialized]->flags & (PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE))
            fds[fds_count++] = cargo[serialized]->generic.fd;
    }

    char* blob = malloc(blob_size);
    if (!blob) {
        ret = -PAL_ERROR_NOMEM;
        goto out_free;
    }
    struct pipe_handles_hdr* hdr = (struct pipe_handles_hdr*)blob;
    hdr->magic = PIPE_HANDLES_MAGIC;
    hdr->cargo_count = cargo_count;
    hdr->meta_size = meta_size;
    hdr->reserved = 0;
    char* cur = blob + sizeof(*hdr);
    for (size_t i = 0; i < cargo_count; i++) {
        struct pipe_cargo_hdr ch = {
            .has_fd = !!(cargo[i]->flags & (PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE)),
            .data_size = sizes[i],
        };
        memcpy(cur, &ch, sizeof(ch));
        cur += sizeof(ch);
        memcpy(cur, datas[i], sizes[i]);
        cur += sizes[i];
    }
    if (meta_size)
        memcpy(cur, meta, meta_size);

    int memfd = DO_SYSCALL(memfd_create, "gramine-pipe-handles", MFD_CLOEXEC);
    if (memfd < 0) {
        ret = unix_to_pal_error(memfd);
        goto out_blob;
    }
    for (size_t off = 0; off < blob_size;) {
        ssize_t w = DO_SYSCALL(write, memfd, blob + off, blob_size - off);
        if (w < 0) {
            ret = unix_to_pal_error(w);
            goto out_memfd;
        }
        off += w;
    }
    fds[0] = memfd;

    struct iovec iov = {.iov_base = (void*)buffer, .iov_len = count};
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * (1 + PAL_STREAM_MAX_HANDLES))];
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = CMSG_SPACE(sizeof(int) * fds_count),
    };
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds_count);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fds_count);

    ssize_t bytes = DO_SYSCALL(sendmsg, handle->pipe.fd, &msg, MSG_NOSIGNAL);
    ret = bytes < 0 ? unix_to_pal_error(bytes) : (int)bytes;
    if (bytes >= 0 && bytes > INT_MAX)
        ret = INT_MAX;
out_memfd:
    DO_SYSCALL(close, memfd);
out_blob:
    free(blob);
out_free:
    for (size_t i = 0; i < serialized; i++)
        free(datas[i]);
    return ret;
}

static void close_fds(const int* fds, size_t from, size_t count) {
    for (size_t i = from; i < count; i++)
        DO_SYSCALL(close, fds[i]);
}

static int64_t pipe_readhandles(PAL_HANDLE handle, void* buffer, size_t count,
                                PAL_HANDLE* out_cargo, size_t* cargo_count, void* meta,
                                size_t* meta_size, bool* out_truncated) {
    if (handle->hdr.type != PAL_TYPE_PIPECLI && handle->hdr.type != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;

    size_t cargo_cap = *cargo_count;
    size_t meta_cap = *meta_size;
    *cargo_count = 0;
    *meta_size = 0;
    *out_truncated = false;

    struct iovec iov = {.iov_base = buffer, .iov_len = count};
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * (1 + PAL_STREAM_MAX_HANDLES))];
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t bytes = DO_SYSCALL(recvmsg, handle->pipe.fd, &msg, MSG_CMSG_CLOEXEC);
    if (bytes < 0)
        return unix_to_pal_error(bytes);

    int fds[1 + PAL_STREAM_MAX_HANDLES];
    size_t fds_count = 0;
    /* the sender attaches one SCM_RIGHTS control message (the kernel merges none) */
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n && fds_count < 1 + PAL_STREAM_MAX_HANDLES; i++)
            memcpy(&fds[fds_count++], CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
    }
    if (fds_count == 0)
        return bytes;
    if (msg.msg_flags & MSG_CTRUNC) {
        close_fds(fds, 0, fds_count);
        *out_truncated = true;
        return bytes;
    }

    /* the first fd is the memfd with the serialised handles and the blob */
    int memfd = fds[0];
    struct stat st;
    char* blob = NULL;
    int ret = DO_SYSCALL(fstat, memfd, &st);
    if (ret < 0 || st.st_size < (int64_t)sizeof(struct pipe_handles_hdr))
        goto drop_all;
    blob = malloc(st.st_size);
    if (!blob)
        goto drop_all;
    for (int64_t off = 0; off < (int64_t)st.st_size;) {
        ssize_t r = DO_SYSCALL(pread64, memfd, blob + off, st.st_size - off, off);
        if (r <= 0)
            goto drop_all;
        off += r;
    }
    struct pipe_handles_hdr* hdr = (struct pipe_handles_hdr*)blob;
    if (hdr->magic != PIPE_HANDLES_MAGIC || hdr->cargo_count > PAL_STREAM_MAX_HANDLES)
        goto drop_all;

    const char* cur = blob + sizeof(*hdr);
    const char* end = blob + st.st_size;
    size_t next_fd = 1;
    for (size_t i = 0; i < hdr->cargo_count; i++) {
        struct pipe_cargo_hdr ch;
        if (cur + sizeof(ch) > end)
            goto drop_all;
        memcpy(&ch, cur, sizeof(ch));
        cur += sizeof(ch);
        if (cur + ch.data_size > end || (ch.has_fd && next_fd >= fds_count))
            goto drop_all;
        PAL_HANDLE hdl = NULL;
        ret = handle_deserialize(&hdl, cur, ch.data_size);
        cur += ch.data_size;
        int fd = ch.has_fd ? fds[next_fd++] : -1;
        if (ret < 0) {
            if (fd >= 0)
                DO_SYSCALL(close, fd);
            *out_truncated = true;
            continue;
        }
        if (fd >= 0) {
            hdl->generic.fd = fd;
        } else {
            hdl->flags &= ~(PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE);
            hdl->generic.fd = -1;
        }
        if (*cargo_count < cargo_cap) {
            out_cargo[(*cargo_count)++] = hdl;
        } else {
            _PalObjectDestroy(hdl);
            *out_truncated = true;
        }
    }
    if (hdr->meta_size) {
        if ((size_t)(end - cur) < hdr->meta_size)
            goto drop_rest;
        size_t copy = MIN((size_t)hdr->meta_size, meta_cap);
        if (copy)
            memcpy(meta, cur, copy);
        *meta_size = copy;
        if (copy < hdr->meta_size)
            *out_truncated = true;
    }
drop_rest:
    close_fds(fds, next_fd, fds_count);
    DO_SYSCALL(close, memfd);
    free(blob);
    return bytes;

drop_all:
    /* not a Saguaro sender, or a damaged blob: the fds are dropped, as Linux drops unread ones */
    close_fds(fds, 0, fds_count);
    free(blob);
    *out_truncated = true;
    return bytes;
}

/*!
 * \brief Destroy pipe (close host FD and free all objects).
 *
 * \param handle  PAL handle of type `pipesrv`, `pipecli`, or `pipe`.
 */
static void pipe_destroy(PAL_HANDLE handle) {
    assert(handle->hdr.type == PAL_TYPE_PIPESRV || handle->hdr.type == PAL_TYPE_PIPECLI
            || handle->hdr.type == PAL_TYPE_PIPE);

    int ret = DO_SYSCALL(close, handle->pipe.fd);
    if (ret < 0) {
        log_error("closing pipe host fd %d failed: %s", handle->pipe.fd, unix_strerror(ret));
        /* We cannot do anything about it anyway... */
    }

    free(handle);
}

/*!
 * \brief Shut down pipe.
 *
 * \param handle       PAL handle of type `pipesrv`, `pipecli`, or `pipe`.
 * \param delete_mode  See #pal_delete_mode.
 *
 * \returns 0 on success, negative PAL error code otherwise.
 */
static int pipe_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    int shutdown;
    switch (delete_mode) {
        case PAL_DELETE_ALL:
            shutdown = SHUT_RDWR;
            break;
        case PAL_DELETE_READ:
            shutdown = SHUT_RD;
            break;
        case PAL_DELETE_WRITE:
            shutdown = SHUT_WR;
            break;
        default:
            return -PAL_ERROR_INVAL;
    }

    DO_SYSCALL(shutdown, handle->pipe.fd, shutdown);
    return 0;
}

/*!
 * \brief Retrieve attributes of PAL handle.
 *
 * \param      handle  PAL handle of type `pipesrv`, `pipecli`, or `pipe`.
 * \param[out] attr    User-supplied buffer to store handle's current attributes.
 *
 * \returns 0 on success, negative PAL error code otherwise.
 */
static int pipe_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    int ret;

    attr->handle_type  = handle->hdr.type;
    attr->nonblocking  = handle->pipe.nonblocking;

    /* get number of bytes available for reading (doesn't make sense for "listening" pipes) */
    attr->pending_size = 0;
    if (handle->hdr.type != PAL_TYPE_PIPESRV) {
        int val;
        ret = DO_SYSCALL(ioctl, handle->pipe.fd, FIONREAD, &val);
        if (ret < 0)
            return unix_to_pal_error(ret);

        attr->pending_size = val;
    }

    return 0;
}

/*!
 * \brief Set attributes of PAL handle.
 *
 * \param handle  PAL handle of type `pipesrv`, `pipecli`, or `pipe`.
 * \param attr    User-supplied buffer with new handle's attributes.
 *
 * \returns 0 on success, negative PAL error code otherwise.
 *
 * Currently only `nonblocking` attribute can be set.
 */
static int pipe_attrsetbyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    bool* nonblocking = &handle->pipe.nonblocking;

    if (attr->nonblocking != *nonblocking) {
        int ret = DO_SYSCALL(fcntl, handle->pipe.fd, F_SETFL, attr->nonblocking ? O_NONBLOCK : 0);
        if (ret < 0)
            return unix_to_pal_error(ret);

        *nonblocking = attr->nonblocking;
    }

    return 0;
}

struct handle_ops g_pipe_ops = {
    .open           = &pipe_open,
    .waitforclient  = &pipe_waitforclient,
    .read           = &pipe_read,
    .write          = &pipe_write,
    .writehandles   = &pipe_writehandles,
    .readhandles    = &pipe_readhandles,
    .destroy        = &pipe_destroy,
    .delete         = &pipe_delete,
    .attrquerybyhdl = &pipe_attrquerybyhdl,
    .attrsetbyhdl   = &pipe_attrsetbyhdl,
};
