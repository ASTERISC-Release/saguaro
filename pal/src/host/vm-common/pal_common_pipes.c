/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * This file contains APIs to handle pipes.
 *
 * Two pipes (two ends of the same connection) share a single buffer object with one ring per
 * direction (see `struct pal_handle_inner_pipe_buf`). The connecting end creates it (so that, as
 * with a Unix socket, it can write before the server accepts), the accepting end takes it over, and
 * it is destroyed when the last of the two pipes is closed. Each end writes its own ring and reads
 * the other one's, so both ends may write at the same time, as on a Unix socket; formerly there
 * was one ring and an end that wrote and then read got its own bytes back.
 */

#include "api.h"
#include "list.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "spinlock.h"

#include "kernel_sched.h"

/* Global lock for all connecting operations: waiting for clients, connecting to server, etc.
 * This lock also protects `pal_handle::pipe.pipe_buf` reference. */
spinlock_t g_connecting_pipes_lock = INIT_SPINLOCK_UNLOCKED;

/* List of "pipesrv" named pipes; this list is used to find corresponding "pipesrv" pipe for each
 * connecting pipe during pipe_connect() */
LISTP_TYPE(pal_handle) g_server_pipes_list = LISTP_INIT;

/* List of "pipe" connecting pipes; this list is used to find corresponding "connecting" pipe for
 * each accepting pipe during pipe_waitforclient() */
LISTP_TYPE(pal_handle) g_connecting_pipes_list = LISTP_INIT;

static int pipe_listen(struct pal_handle** handle, const char* name, pal_stream_options_t options) {
    int ret;

    struct pal_handle* pipe = calloc(1, sizeof(*pipe));
    if (!pipe)
        return -PAL_ERROR_NOMEM;

    pipe->hdr.type = PAL_TYPE_PIPESRV;
    pipe->flags = 0; /* cannot read or write on the server pipe */
    pipe->pipe.nonblocking = !!(options & PAL_OPTION_NONBLOCK);
    memcpy(&pipe->pipe.name, name, strlen(name) + 1);

    spinlock_lock(&g_connecting_pipes_lock);

    struct pal_handle* server_pipe;
    LISTP_FOR_EACH_ENTRY(server_pipe, &g_server_pipes_list, list) {
        if (strcmp(server_pipe->pipe.name, name) == 0) {
            ret = -PAL_ERROR_STREAMEXIST;
            goto out;
        }
    }
    LISTP_ADD(pipe, &g_server_pipes_list, list);

    *handle = pipe;
    ret = 0;
out:
    spinlock_unlock(&g_connecting_pipes_lock);
    if (ret < 0)
        free(pipe);
    return ret;
}

static struct pal_handle_inner_pipe_buf* pipe_buf_alloc(void) {
    struct pal_handle_inner_pipe_buf* pipe_buf = calloc(1, sizeof(*pipe_buf));
    if (!pipe_buf)
        return NULL;
    for (int i = 0; i < 2; i++) {
        pipe_buf->ring[i].open_write = pipe_buf->ring[i].open_read = true;
        pipe_buf->ring[i].write_pos = pipe_buf->ring[i].read_pos = 0;
    }
    spinlock_init(&pipe_buf->lock);
    /* one reference for the connecting end, one held for the end that will accept it */
    pipe_buf->refcount = 2;
    return pipe_buf;
}

/* the ring an end writes: the connecting end (PAL_TYPE_PIPE) ring 0, the accepted end ring 1 */
static struct pal_pipe_ring* write_ring_of(struct pal_handle_inner_pipe_buf* pipe_buf,
                                           int type) {
    return &pipe_buf->ring[type == PAL_TYPE_PIPE ? 0 : 1];
}

static struct pal_pipe_ring* read_ring_of(struct pal_handle_inner_pipe_buf* pipe_buf,
                                          int type) {
    return &pipe_buf->ring[type == PAL_TYPE_PIPE ? 1 : 0];
}

static void ring_wake_all(struct pal_pipe_ring* ring) {
    sched_thread_wakeup(&ring->reader_futex);
    sched_thread_wakeup(&ring->writer_futex);
}

/* Handles ride through a pipe to another process only where handle objects are shared between
 * processes: the VM PAL (share_refs, vm_handle_release_share in pal_process.c; the same guard as
 * pal/src/pal_object.c). The TDX PAL has no processes. */
#if defined(PAL_HOST_VM) && (defined(__x86_64__) || defined(__aarch64__))
#define PIPE_HANDLES_SHARED 1
#endif

/* drops the handles of a queue entry nobody will receive (or beyond a reader's capacity) */
static void ring_handles_drop(struct pal_pipe_ring_handles* e, size_t from) {
    for (size_t i = from; i < e->count; i++) {
        /* the reference the sender took for the receiver (see pal_common_pipe_writehandles) */
#ifdef PIPE_HANDLES_SHARED
        if (!vm_handle_release_share(e->cargo[i]))
#endif
            _PalObjectDestroy(e->cargo[i]);
    }
}

static void ring_handles_free(struct pal_pipe_ring_handles* e) {
    free(e->cargo);
    free(e->meta);
    e->cargo = NULL;
    e->meta = NULL;
    e->count = 0;
}

static void ring_handles_drop_all(struct pal_pipe_ring* ring) {
    while (ring->hq_head != ring->hq_tail) {
        struct pal_pipe_ring_handles* e = &ring->hq[ring->hq_head++ % PIPE_RING_HANDLE_SLOTS];
        ring_handles_drop(e, 0);
        ring_handles_free(e);
    }
}

/* closes the end of type `type`: its peer reads what is left and then end-of-stream, and its
 * peer's writes fail; must hold pipe_buf->lock */
static void pipe_buf_close_end(struct pal_handle_inner_pipe_buf* pipe_buf,
                               int type) {
    write_ring_of(pipe_buf, type)->open_write = false;
    read_ring_of(pipe_buf, type)->open_read = false;
    ring_wake_all(&pipe_buf->ring[0]);
    ring_wake_all(&pipe_buf->ring[1]);
    if (pipe_buf->poll_waiting)
        sched_streams_wake();
}

/* Drops the reference of the end of type `type`; the other end, if any, sees the pipe closed. The
 * count changes under the buffer's lock: the two ends may be closed at the same time (by two
 * processes), and the last one must free the buffer only after the other one has released the
 * lock. */
static void pipe_buf_put(struct pal_handle_inner_pipe_buf* pipe_buf, int type) {
    spinlock_lock(&pipe_buf->lock);
    int new_count = --pipe_buf->refcount;
    if (new_count < 0) {
        log_error("Reference count dropped below 0 at %s:%d", __FILE_NAME__, __LINE__);
        BUG();
    } else if (new_count > 0) {
        pipe_buf_close_end(pipe_buf, type);
        spinlock_unlock(&pipe_buf->lock);
    } else {
        ring_handles_drop_all(&pipe_buf->ring[0]);
        ring_handles_drop_all(&pipe_buf->ring[1]);
        spinlock_unlock(&pipe_buf->lock);
        free(pipe_buf);
    }
}

static int pipe_connect(struct pal_handle** handle, const char* name,
                        pal_stream_options_t options) {
    int ret;

    struct pal_handle* pipe = calloc(1, sizeof(*pipe));
    if (!pipe)
        return -PAL_ERROR_NOMEM;

    struct pal_handle_inner_pipe_buf* pipe_buf = pipe_buf_alloc();
    if (!pipe_buf) {
        free(pipe);
        return -PAL_ERROR_NOMEM;
    }

    pipe->hdr.type = PAL_TYPE_PIPE;
    pipe->flags = PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;
    pipe->pipe.nonblocking = !!(options & PAL_OPTION_NONBLOCK);
    memcpy(&pipe->pipe.name, name, strlen(name) + 1);
    pipe->pipe.pipe_buf = pipe_buf;

    spinlock_lock(&g_connecting_pipes_lock);

    struct pal_handle* found_server_pipe = NULL;
    struct pal_handle* server_pipe;
    LISTP_FOR_EACH_ENTRY(server_pipe, &g_server_pipes_list, list) {
        if (strcmp(server_pipe->pipe.name, name) == 0) {
            found_server_pipe = server_pipe;
            break;
        }
    }

    if (!found_server_pipe) {
        ret = -PAL_ERROR_CONNFAILED;
        goto out;
    }

    /* notify the other end's waitforclient() and any other waiting events (select/poll) */
    LISTP_ADD(pipe, &g_connecting_pipes_list, list);
    if (found_server_pipe->pipe.connect_poll_waiting)
        sched_streams_wake();
    sched_thread_wakeup(&found_server_pipe->pipe.connect_futex);

    *handle = pipe;
    ret = 0;
out:
    spinlock_unlock(&g_connecting_pipes_lock);
    if (ret < 0) {
        free(pipe_buf);
        free(pipe);
    }
    return ret;
}

int pal_common_pipe_open(struct pal_handle** handle, const char* type, const char* uri,
                         enum pal_access access, pal_share_flags_t share,
                         enum pal_create_mode create, pal_stream_options_t options) {
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

int pal_common_pipe_waitforclient(struct pal_handle* server, struct pal_handle** client,
                                  pal_stream_options_t options) {
    int ret;

    if (server->hdr.type != PAL_TYPE_PIPESRV)
        return -PAL_ERROR_NOTSERVER;

    struct pal_handle* pipe = calloc(1, sizeof(*pipe));
    if (!pipe)
        return -PAL_ERROR_NOMEM;

    pipe->hdr.type = PAL_TYPE_PIPECLI;
    pipe->flags = PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;
    pipe->pipe.nonblocking = !!(options & PAL_OPTION_NONBLOCK);
    memcpy(&pipe->pipe.name, &server->pipe.name, sizeof(pipe->pipe.name));

    /* emulate accept(): wait for the first pipe connecting on server's name */
    spinlock_lock(&g_connecting_pipes_lock);

    struct pal_handle* found_connecting_pipe = NULL;

    while (!found_connecting_pipe) {
        struct pal_handle* connecting_pipe = NULL;
        LISTP_FOR_EACH_ENTRY(connecting_pipe, &g_connecting_pipes_list, list) {
            if (strcmp(connecting_pipe->pipe.name, server->pipe.name) == 0) {
                found_connecting_pipe = connecting_pipe;
                break;
            }
        }

        if (!found_connecting_pipe) {
            if (server->pipe.nonblocking) {
                ret = -PAL_ERROR_TRYAGAIN;
                goto out;
            }
            ret = sched_thread_wait_interruptible(&server->pipe.connect_futex,
                                                  &g_connecting_pipes_lock);
            if (ret < 0)
                goto out;
        }
    }
    assert(found_connecting_pipe);

    LISTP_DEL(found_connecting_pipe, &g_connecting_pipes_list, list);

    /* take over the reference the connecting end holds for us */
    pipe->pipe.pipe_buf = found_connecting_pipe->pipe.pipe_buf;
    if (!found_connecting_pipe->pipe.closed_pending) {
        /* the client still lives; it keeps its handle */
        found_connecting_pipe = NULL;
    }

    *client = pipe;
    ret = 0;
out:
    spinlock_unlock(&g_connecting_pipes_lock);
    if (ret == 0 && found_connecting_pipe) {
        /* The client closed before being accepted (see pal_common_pipe_destroy); the queue owned
         * its handle until now. This end reads what it wrote, then end-of-stream. */
        free(found_connecting_pipe);
    }
    if (ret < 0)
        free(pipe);
    return ret;
}

/* Reads from `ring` under pipe_buf->lock. Handles queued at the read position are delivered into
 * out_cargo (capacity *cargo_count; the rest are dropped and *out_truncated set) with their blob;
 * a read never runs past the position of queued handles. */
static ssize_t ring_read_locked(struct pal_handle* handle, struct pal_handle_inner_pipe_buf* pipe_buf,
                                struct pal_pipe_ring* ring, char* buf, size_t len,
                                struct pal_handle** out_cargo, size_t* cargo_count, void* meta,
                                size_t* meta_size, bool* out_truncated) {
    ssize_t bytes;
    size_t cargo_cap = cargo_count ? *cargo_count : 0;
    size_t meta_cap = meta_size ? *meta_size : 0;
    if (cargo_count)
        *cargo_count = 0;
    if (meta_size)
        *meta_size = 0;
    if (out_truncated)
        *out_truncated = false;

    while (ring->read_pos == ring->write_pos) {
        if (!ring->open_write) {
            /* the other end closed its writing side, no sense in waiting -- always return 0 */
            bytes = 0;
            goto out;
        }

        if (handle->pipe.nonblocking) {
            bytes = -PAL_ERROR_TRYAGAIN;
            goto out;
        }

        int wret = sched_thread_wait_interruptible(&ring->reader_futex, &pipe_buf->lock);
        if (wret < 0) {
            bytes = wret;
            goto out;
        }
    }

    assert(ring->read_pos != ring->write_pos);
    assert(ring->write_pos - ring->read_pos <= PIPE_BUF_SIZE);

    /* handles attached at this position come with this read; a later attachment bounds it */
    while (ring->hq_head != ring->hq_tail) {
        struct pal_pipe_ring_handles* e = &ring->hq[ring->hq_head % PIPE_RING_HANDLE_SLOTS];
        if (e->pos > ring->read_pos) {
            len = MIN(len, (size_t)(e->pos - ring->read_pos));
            break;
        }
        if (e->pos < ring->read_pos) {
            /* cannot happen (reads stop at attachments); drop a stale entry rather than deliver
             * it with unrelated bytes */
            ring_handles_drop(e, 0);
            ring_handles_free(e);
            ring->hq_head++;
            continue;
        }
        if (cargo_count && *cargo_count) {
            /* a second attachment at the same position: it comes with the next read */
            break;
        }
        size_t i = 0;
        for (; i < e->count && i < cargo_cap; i++)
            out_cargo[i] = e->cargo[i];
        if (cargo_count)
            *cargo_count = i;
        if (i < e->count) {
            ring_handles_drop(e, i);
            if (out_truncated)
                *out_truncated = true;
        }
        if (e->meta_size) {
            size_t copy = MIN(e->meta_size, meta_cap);
            if (copy)
                memcpy(meta, e->meta, copy);
            if (meta_size)
                *meta_size = copy;
            if (copy < e->meta_size && out_truncated)
                *out_truncated = true;
        }
        ring_handles_free(e);
        ring->hq_head++;
        sched_thread_wakeup(&ring->writer_futex);
    }

    bytes = 0;
    while (bytes < (ssize_t)len && ring->read_pos != ring->write_pos) {
        /* limited by three factors: how much is requested by caller, how much is available in the
         * ring, and how much left until wrap around in the ring */
        size_t x = MIN(MIN(len - bytes, ring->write_pos - ring->read_pos),
                       ALIGN_UP(ring->read_pos + 1, PIPE_BUF_SIZE) - ring->read_pos);

        memcpy(&buf[bytes], &ring->buf[ring->read_pos % PIPE_BUF_SIZE], x);

        ring->read_pos += x;
        bytes += x;
    }

out:
    if (pipe_buf->poll_waiting)
        sched_streams_wake();
    sched_thread_wakeup(&ring->writer_futex);
    return bytes;
}

static int64_t pipe_read_common(struct pal_handle* handle, uint64_t len, void* buffer,
                                struct pal_handle** out_cargo, size_t* cargo_count, void* meta,
                                size_t* meta_size, bool* out_truncated) {
    if (handle->hdr.type != PAL_TYPE_PIPECLI && handle->hdr.type != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;

    if (!(handle->flags & PAL_HANDLE_FD_READABLE))
        return -PAL_ERROR_INVAL;

    spinlock_lock(&g_connecting_pipes_lock);
    struct pal_handle_inner_pipe_buf* pipe_buf = handle->pipe.pipe_buf;
    spinlock_unlock(&g_connecting_pipes_lock);

    if (!pipe_buf)
        return -PAL_ERROR_NOTCONNECTION;

    spinlock_lock(&pipe_buf->lock);
    struct pal_pipe_ring* ring = read_ring_of(pipe_buf, handle->hdr.type);
    ssize_t bytes = ring_read_locked(handle, pipe_buf, ring, buffer, len, out_cargo, cargo_count,
                                     meta, meta_size, out_truncated);
    spinlock_unlock(&pipe_buf->lock);
    return bytes;
}

int64_t pal_common_pipe_read(struct pal_handle* handle, uint64_t offset, uint64_t len,
                             void* buffer) {
    if (offset)
        return -PAL_ERROR_INVAL;
    /* a plain read drops handles attached to the bytes it takes, as Linux does without a control
     * buffer */
    size_t none = 0;
    return pipe_read_common(handle, len, buffer, NULL, &none, NULL, NULL, NULL);
}

int64_t pal_common_pipe_readhandles(struct pal_handle* handle, void* buffer, size_t len,
                                    struct pal_handle** out_cargo, size_t* cargo_count,
                                    void* meta, size_t* meta_size, bool* out_truncated) {
    return pipe_read_common(handle, len, buffer, out_cargo, cargo_count, meta, meta_size,
                            out_truncated);
}

/* Writes to `ring` under pipe_buf->lock (which it drops while waiting for room). */
static ssize_t ring_write_locked(struct pal_handle* handle, struct pal_handle_inner_pipe_buf* pipe_buf,
                                 struct pal_pipe_ring* ring, const char* buf, size_t len) {
    ssize_t bytes = 0;
    /* must guarantee that PIPE_BUF_SIZE bytes are written atomically (for a blocking pipe) */
    while (bytes < (ssize_t)len) {
        if (!ring->open_read) {
            /* the other end closed its reading side, this write must fail */
            if (!bytes)
                bytes = -PAL_ERROR_CONNFAILED_PIPE;
            goto out;
        }

        while (ring->write_pos - ring->read_pos == PIPE_BUF_SIZE) {
            if (!ring->open_read) {
                if (!bytes)
                    bytes = -PAL_ERROR_CONNFAILED_PIPE;
                goto out;
            }

            if (handle->pipe.nonblocking) {
                if (!bytes)
                    bytes = -PAL_ERROR_TRYAGAIN;
                goto out;
            }

            if (pipe_buf->poll_waiting)
                sched_streams_wake();
            sched_thread_wakeup(&ring->reader_futex);
            int wret = sched_thread_wait_interruptible(&ring->writer_futex, &pipe_buf->lock);
            if (wret < 0) {
                if (!bytes)
                    bytes = wret; /* a partial write counts, as on Linux */
                goto out;
            }
        }

        /* limited by three factors: how much is requested by caller, how much left for writing in
         * the ring, and how much left until wrap around in the ring */
        size_t x = MIN(MIN(len - bytes, PIPE_BUF_SIZE - (ring->write_pos - ring->read_pos)),
                       ALIGN_UP(ring->write_pos + 1, PIPE_BUF_SIZE) - ring->write_pos);

        memcpy(&ring->buf[ring->write_pos % PIPE_BUF_SIZE], &buf[bytes], x);

        ring->write_pos += x;
        bytes += x;
    }

out:
    if (pipe_buf->poll_waiting)
        sched_streams_wake();
    sched_thread_wakeup(&ring->reader_futex);
    return bytes;
}

static int pipe_write_prepare(struct pal_handle* handle, uint64_t offset,
                              struct pal_handle_inner_pipe_buf** out_pipe_buf) {
    if (offset)
        return -PAL_ERROR_INVAL;

    if (handle->hdr.type != PAL_TYPE_PIPECLI && handle->hdr.type != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;

    if (!(handle->flags & PAL_HANDLE_FD_WRITABLE))
        return -PAL_ERROR_INVAL;

    spinlock_lock(&g_connecting_pipes_lock);
    struct pal_handle_inner_pipe_buf* pipe_buf = handle->pipe.pipe_buf;
    spinlock_unlock(&g_connecting_pipes_lock);

    if (!pipe_buf)
        return -PAL_ERROR_NOTCONNECTION;
    *out_pipe_buf = pipe_buf;
    return 0;
}

int64_t pal_common_pipe_write(struct pal_handle* handle, uint64_t offset, uint64_t len,
                              const void* buffer) {
    struct pal_handle_inner_pipe_buf* pipe_buf;
    int ret = pipe_write_prepare(handle, offset, &pipe_buf);
    if (ret < 0)
        return ret;

    spinlock_lock(&pipe_buf->lock);
    struct pal_pipe_ring* ring = write_ring_of(pipe_buf, handle->hdr.type);
    ssize_t bytes = ring_write_locked(handle, pipe_buf, ring, buffer, len);
    spinlock_unlock(&pipe_buf->lock);
    return bytes;
}

int64_t pal_common_pipe_writehandles(struct pal_handle* handle, const void* buffer, size_t len,
                                     struct pal_handle** cargo, size_t cargo_count,
                                     const void* meta, size_t meta_size) {
    /* a message without handles and without a description is a plain write; a description alone
     * (the LibOS passes a graphics host handle, which has no PAL handle) travels like handles */
    if (cargo_count == 0 && meta_size == 0)
        return pal_common_pipe_write(handle, 0, len, buffer);
    if (cargo_count > PAL_STREAM_MAX_HANDLES)
        return -PAL_ERROR_INVAL;
#ifndef PIPE_HANDLES_SHARED
    __UNUSED(meta);
    __UNUSED(meta_size);
    return -PAL_ERROR_NOTIMPLEMENTED;
#else
    for (size_t i = 0; i < cargo_count; i++) {
        /* process endpoints, threads and events are per process (see vm_process_send_handle) */
        if (cargo[i]->hdr.type == PAL_TYPE_PROCESS || cargo[i]->hdr.type == PAL_TYPE_THREAD
                || cargo[i]->hdr.type == PAL_TYPE_EVENT)
            return -PAL_ERROR_NOTIMPLEMENTED;
    }

    struct pal_handle_inner_pipe_buf* pipe_buf;
    int ret = pipe_write_prepare(handle, 0, &pipe_buf);
    if (ret < 0)
        return ret;

    struct pal_handle** cargo_copy = malloc((cargo_count ? cargo_count : 1) * sizeof(*cargo_copy));
    void* meta_copy = meta_size ? malloc(meta_size) : NULL;
    if (!cargo_copy || (meta_size && !meta_copy)) {
        free(cargo_copy);
        free(meta_copy);
        return -PAL_ERROR_NOMEM;
    }
    memcpy(cargo_copy, cargo, cargo_count * sizeof(*cargo_copy));
    if (meta_size)
        memcpy(meta_copy, meta, meta_size);

    spinlock_lock(&pipe_buf->lock);
    struct pal_pipe_ring* ring = write_ring_of(pipe_buf, handle->hdr.type);
    ssize_t bytes;
    while (ring->hq_tail - ring->hq_head == PIPE_RING_HANDLE_SLOTS) {
        if (!ring->open_read) {
            bytes = -PAL_ERROR_CONNFAILED_PIPE;
            goto out_err;
        }
        if (handle->pipe.nonblocking) {
            bytes = -PAL_ERROR_TRYAGAIN;
            goto out_err;
        }
        bytes = sched_thread_wait_interruptible(&ring->writer_futex, &pipe_buf->lock);
        if (bytes < 0)
            goto out_err;
    }
    /* the receiver shares the handle objects: one reference each, as PalSendHandle gives */
    for (size_t i = 0; i < cargo_count; i++)
        __atomic_add_fetch(&cargo_copy[i]->share_refs, 1, __ATOMIC_ACQ_REL);
    struct pal_pipe_ring_handles* e = &ring->hq[ring->hq_tail % PIPE_RING_HANDLE_SLOTS];
    e->pos = ring->write_pos;
    e->cargo = cargo_copy;
    e->count = cargo_count;
    e->meta = meta_copy;
    e->meta_size = meta_size;
    ring->hq_tail++;

    bytes = ring_write_locked(handle, pipe_buf, ring, buffer, len);
    if (bytes <= 0 && ring->hq_tail - ring->hq_head > 0
            && &ring->hq[(ring->hq_tail - 1) % PIPE_RING_HANDLE_SLOTS] == e && e->cargo == cargo_copy) {
        /* nothing was written: take the attachment back */
        ring->hq_tail--;
        ring_handles_drop(e, 0);
        ring_handles_free(e);
    }
    spinlock_unlock(&pipe_buf->lock);
    return bytes;

out_err:
    spinlock_unlock(&pipe_buf->lock);
    free(cargo_copy);
    free(meta_copy);
    return bytes;
#endif
}

void pal_common_pipe_destroy(struct pal_handle* handle) {
    assert(handle->hdr.type == PAL_TYPE_PIPESRV || handle->hdr.type == PAL_TYPE_PIPECLI
            || handle->hdr.type == PAL_TYPE_PIPE);

    bool found_server_pipe = false;
    bool found_connecting_pipe = false;
    struct pal_handle_inner_pipe_buf* pipe_buf = NULL;
    /* clients queued for a server that is going away; they will never be accepted */
    LISTP_TYPE(pal_handle) orphans = LISTP_INIT;

    spinlock_lock(&g_connecting_pipes_lock);

    if (handle->hdr.type == PAL_TYPE_PIPESRV) {
        struct pal_handle* server_pipe;
        LISTP_FOR_EACH_ENTRY(server_pipe, &g_server_pipes_list, list) {
            if (server_pipe == handle) {
                found_server_pipe = true;
                break;
            }
        }
    }
    if (handle->hdr.type == PAL_TYPE_PIPE) {
        struct pal_handle* connecting_pipe;
        LISTP_FOR_EACH_ENTRY(connecting_pipe, &g_connecting_pipes_list, list) {
            if (connecting_pipe == handle) {
                found_connecting_pipe = true;
                break;
            }
        }
    }

    if (found_server_pipe) {
        LISTP_DEL(handle, &g_server_pipes_list, list);
        struct pal_handle* connecting_pipe;
        struct pal_handle* tmp;
        LISTP_FOR_EACH_ENTRY_SAFE(connecting_pipe, tmp, &g_connecting_pipes_list, list) {
            if (strcmp(connecting_pipe->pipe.name, handle->pipe.name) == 0) {
                LISTP_DEL(connecting_pipe, &g_connecting_pipes_list, list);
                LISTP_ADD(connecting_pipe, &orphans, list);
            }
        }
    }
    if (found_connecting_pipe) {
        /* A client that closes before the server accepted it stays queued, so that the server's
         * accept still completes and reads what the client wrote, as with a Unix socket. Without
         * this, a server waiting in another process would never see a short-lived client. The
         * queue owns the handle from now on; waitforclient (or the server's destroy) frees it. */
        handle->pipe.closed_pending = true;
        pipe_buf = handle->pipe.pipe_buf;
        spinlock_unlock(&g_connecting_pipes_lock);
        pipe_buf_put(pipe_buf, PAL_TYPE_PIPE); /* the client's reference; the server's stays */
        return;
    }

    if (handle->hdr.type == PAL_TYPE_PIPE || handle->hdr.type == PAL_TYPE_PIPECLI) {
        pipe_buf = handle->pipe.pipe_buf;
    }
    spinlock_unlock(&g_connecting_pipes_lock);

    struct pal_handle* orphan;
    struct pal_handle* tmp;
    LISTP_FOR_EACH_ENTRY_SAFE(orphan, tmp, &orphans, list) {
        LISTP_DEL(orphan, &orphans, list);
        /* drop the reference held for the accepting end: a live client sees the pipe closed */
        pipe_buf_put(orphan->pipe.pipe_buf, PAL_TYPE_PIPECLI);
        if (orphan->pipe.closed_pending)
            free(orphan);
    }

    if (pipe_buf)
        pipe_buf_put(pipe_buf, handle->hdr.type);

    free(handle);
}

int pal_common_pipe_delete(struct pal_handle* handle, enum pal_delete_mode delete_mode) {
    /* emulate closing specified end of the pipe */
    switch (delete_mode) {
        case PAL_DELETE_ALL:
            handle->flags = 0;
            break;
        case PAL_DELETE_READ:
            handle->flags &= ~PAL_HANDLE_FD_READABLE;
            break;
        case PAL_DELETE_WRITE:
            handle->flags &= ~PAL_HANDLE_FD_WRITABLE;
            break;
        default:
            return -PAL_ERROR_INVAL;
    }

    struct pal_handle_inner_pipe_buf* pipe_buf = NULL;

    spinlock_lock(&g_connecting_pipes_lock);
    if (handle->pipe.connect_poll_waiting)
        sched_streams_wake();
    pipe_buf = handle->pipe.pipe_buf;
    spinlock_unlock(&g_connecting_pipes_lock);

    if (pipe_buf) {
        spinlock_lock(&pipe_buf->lock);
        if (delete_mode == PAL_DELETE_ALL || delete_mode == PAL_DELETE_READ)
            read_ring_of(pipe_buf, handle->hdr.type)->open_read = false;
        if (delete_mode == PAL_DELETE_ALL || delete_mode == PAL_DELETE_WRITE)
            write_ring_of(pipe_buf, handle->hdr.type)->open_write = false;
        ring_wake_all(&pipe_buf->ring[0]);
        ring_wake_all(&pipe_buf->ring[1]);
        if (pipe_buf->poll_waiting)
            sched_streams_wake();
        spinlock_unlock(&pipe_buf->lock);
    }

    return 0;
}

int pal_common_pipe_attrquerybyhdl(struct pal_handle* handle, PAL_STREAM_ATTR* attr) {
    attr->handle_type  = handle->hdr.type;
    attr->nonblocking  = handle->pipe.nonblocking;
    attr->pending_size = 0;

    struct pal_handle_inner_pipe_buf* pipe_buf = NULL;
    spinlock_lock(&g_connecting_pipes_lock);
    pipe_buf = handle->pipe.pipe_buf;
    spinlock_unlock(&g_connecting_pipes_lock);

    if (pipe_buf) {
        spinlock_lock(&pipe_buf->lock);
        struct pal_pipe_ring* ring = read_ring_of(pipe_buf, handle->hdr.type);
        attr->pending_size = ring->write_pos - ring->read_pos;
        assert(attr->pending_size <= PIPE_BUF_SIZE);
        spinlock_unlock(&pipe_buf->lock);
    }

    return 0;
}

int pal_common_pipe_attrsetbyhdl(struct pal_handle* handle, PAL_STREAM_ATTR* attr) {
    handle->pipe.nonblocking = attr->nonblocking;
    return 0;
}
