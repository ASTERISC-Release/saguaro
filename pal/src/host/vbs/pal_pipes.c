/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Pipes of the vbs PAL proper: "pipe.srv:<name>" and "pipe:<name>" connect inside the one enclave
 * process through two in-memory ring buffers per connection (one per direction). The LibOS uses
 * them for its per-thread pollable events, its IPC and pipe(2). A blocking read or accept sleeps on
 * the stream-change counter (pal_object.c), which every change of a pipe increments.
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"
#include "spinlock.h"

#define VBS_CHAN_SIZE (64 * 1024)

struct vbs_chan {
    spinlock_t lock;
    size_t head, len; /* ring buffer: `len` bytes from `head` */
    int refs;         /* the two ends */
    bool closed;      /* one end is gone */
    char buf[VBS_CHAN_SIZE];
};

struct vbs_pending {
    PAL_HANDLE server_end;
    struct vbs_pending* next;
};

struct vbs_pipe_srv {
    char* name;
    struct vbs_pending* queue;
    struct vbs_pipe_srv* next;
};

static spinlock_t g_srv_lock = INIT_SPINLOCK_UNLOCKED;
static struct vbs_pipe_srv* g_servers;

static struct vbs_chan* chan_new(void) {
    struct vbs_chan* c = calloc(1, sizeof(*c));
    if (c) {
        spinlock_init(&c->lock);
        c->refs = 2;
    }
    return c;
}

static void chan_put(struct vbs_chan* c) {
    spinlock_lock(&c->lock);
    c->closed = true;
    bool last = --c->refs == 0;
    spinlock_unlock(&c->lock);
    if (last)
        free(c);
    vbs_streams_changed();
}

static PAL_HANDLE pipe_end_new(struct vbs_chan* rx, struct vbs_chan* tx, bool nonblocking) {
    PAL_HANDLE h = calloc(1, HANDLE_SIZE(pipe));
    if (!h)
        return NULL;
    init_handle_hdr(h, PAL_TYPE_PIPE);
    h->pipe.rx = rx;
    h->pipe.tx = tx;
    h->pipe.nonblocking = nonblocking;
    return h;
}

static struct vbs_pipe_srv* find_server(const char* name) {
    for (struct vbs_pipe_srv* s = g_servers; s; s = s->next)
        if (!strcmp(s->name, name))
            return s;
    return NULL;
}

static int pipe_listen(PAL_HANDLE* handle, const char* name, pal_stream_options_t options) {
    __UNUSED(options);
    spinlock_lock(&g_srv_lock);
    if (find_server(name)) {
        spinlock_unlock(&g_srv_lock);
        return -PAL_ERROR_STREAMEXIST;
    }
    struct vbs_pipe_srv* s = calloc(1, sizeof(*s));
    PAL_HANDLE h = calloc(1, HANDLE_SIZE(pipesrv));
    if (!s || !h || !(s->name = strdup(name))) {
        spinlock_unlock(&g_srv_lock);
        free(s);
        free(h);
        return -PAL_ERROR_NOMEM;
    }
    s->next = g_servers;
    g_servers = s;
    spinlock_unlock(&g_srv_lock);
    init_handle_hdr(h, PAL_TYPE_PIPESRV);
    h->pipesrv.srv = s;
    *handle = h;
    return 0;
}

static int pipe_connect(PAL_HANDLE* handle, const char* name, pal_stream_options_t options) {
    spinlock_lock(&g_srv_lock);
    struct vbs_pipe_srv* s = find_server(name);
    if (!s) {
        spinlock_unlock(&g_srv_lock);
        return -PAL_ERROR_CONNFAILED;
    }
    struct vbs_chan* c2s = chan_new();
    struct vbs_chan* s2c = chan_new();
    struct vbs_pending* p = calloc(1, sizeof(*p));
    PAL_HANDLE client = pipe_end_new(s2c, c2s, options & PAL_OPTION_NONBLOCK);
    PAL_HANDLE server = pipe_end_new(c2s, s2c, false);
    if (!c2s || !s2c || !p || !client || !server) {
        spinlock_unlock(&g_srv_lock);
        free(c2s);
        free(s2c);
        free(p);
        free(client);
        free(server);
        return -PAL_ERROR_NOMEM;
    }
    p->server_end = server;
    struct vbs_pending** tail = &s->queue;
    while (*tail)
        tail = &(*tail)->next;
    *tail = p;
    spinlock_unlock(&g_srv_lock);
    vbs_streams_changed();
    *handle = client;
    return 0;
}

static int pipe_waitforclient(PAL_HANDLE handle, PAL_HANDLE* client, pal_stream_options_t options) {
    if (HANDLE_TYPE(handle) != PAL_TYPE_PIPESRV)
        return -PAL_ERROR_NOTSERVER;
    struct vbs_pending* p;
    while (1) {
        uint32_t gen = __atomic_load_n(&g_vbs_stream_gen, __ATOMIC_ACQUIRE);
        spinlock_lock(&g_srv_lock);
        p = handle->pipesrv.srv->queue;
        if (p)
            break;
        spinlock_unlock(&g_srv_lock);
        if (options & PAL_OPTION_NONBLOCK)
            return -PAL_ERROR_TRYAGAIN;
        vbs_wait(&g_vbs_stream_gen, gen, UINT64_MAX);
    }
    handle->pipesrv.srv->queue = p->next;
    spinlock_unlock(&g_srv_lock);
    PAL_HANDLE h = p->server_end;
    free(p);
    h->pipe.nonblocking = options & PAL_OPTION_NONBLOCK;
    *client = h;
    return 0;
}

static int pipe_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                     pal_share_flags_t share, enum pal_create_mode create,
                     pal_stream_options_t options) {
    __UNUSED(access);
    __UNUSED(share);
    __UNUSED(create);
    if (!strcmp(type, URI_TYPE_PIPE_SRV))
        return pipe_listen(handle, uri, options);
    if (!strcmp(type, URI_TYPE_PIPE))
        return pipe_connect(handle, uri, options);
    return -PAL_ERROR_INVAL;
}

static int64_t pipe_read(PAL_HANDLE handle, uint64_t offset, uint64_t len, void* buffer) {
    __UNUSED(offset);
    if (HANDLE_TYPE(handle) != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;
    struct vbs_chan* c = handle->pipe.rx;
    while (1) {
        uint32_t gen = __atomic_load_n(&g_vbs_stream_gen, __ATOMIC_ACQUIRE);
        spinlock_lock(&c->lock);
        if (c->len)
            break;
        bool closed = c->closed;
        spinlock_unlock(&c->lock);
        if (closed)
            return 0;
        if (handle->pipe.nonblocking)
            return -PAL_ERROR_TRYAGAIN;
        vbs_wait(&g_vbs_stream_gen, gen, UINT64_MAX);
    }
    size_t n = MIN(len, c->len);
    for (size_t i = 0; i < n; i++)
        ((char*)buffer)[i] = c->buf[(c->head + i) % VBS_CHAN_SIZE];
    c->head = (c->head + n) % VBS_CHAN_SIZE;
    c->len -= n;
    spinlock_unlock(&c->lock);
    vbs_streams_changed(); /* room for a blocked writer */
    return (int64_t)n;
}

static int64_t pipe_write(PAL_HANDLE handle, uint64_t offset, uint64_t len, const void* buffer) {
    __UNUSED(offset);
    if (HANDLE_TYPE(handle) != PAL_TYPE_PIPE)
        return -PAL_ERROR_NOTCONNECTION;
    struct vbs_chan* c = handle->pipe.tx;
    size_t n;
    while (1) {
        uint32_t gen = __atomic_load_n(&g_vbs_stream_gen, __ATOMIC_ACQUIRE);
        spinlock_lock(&c->lock);
        if (c->closed) {
            spinlock_unlock(&c->lock);
            return -PAL_ERROR_CONNFAILED_PIPE;
        }
        n = MIN(len, VBS_CHAN_SIZE - c->len);
        if (n)
            break;
        spinlock_unlock(&c->lock);
        if (handle->pipe.nonblocking)
            return -PAL_ERROR_TRYAGAIN;
        vbs_wait(&g_vbs_stream_gen, gen, UINT64_MAX);
    }
    for (size_t i = 0; i < n; i++)
        c->buf[(c->head + c->len + i) % VBS_CHAN_SIZE] = ((const char*)buffer)[i];
    c->len += n;
    spinlock_unlock(&c->lock);
    vbs_streams_changed();
    return (int64_t)n;
}

static void pipe_destroy(PAL_HANDLE handle) {
    if (HANDLE_TYPE(handle) == PAL_TYPE_PIPE) {
        chan_put(handle->pipe.rx);
        chan_put(handle->pipe.tx);
    } else if (HANDLE_TYPE(handle) == PAL_TYPE_PIPESRV && handle->pipesrv.srv) {
        /* the server object goes with its name (pipe_delete) */
    }
    free(handle);
}

static int pipe_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    __UNUSED(delete_mode);
    if (HANDLE_TYPE(handle) != PAL_TYPE_PIPESRV || !handle->pipesrv.srv)
        return 0;
    spinlock_lock(&g_srv_lock);
    for (struct vbs_pipe_srv** pp = &g_servers; *pp; pp = &(*pp)->next) {
        if (*pp == handle->pipesrv.srv) {
            struct vbs_pipe_srv* s = *pp;
            *pp = s->next;
            while (s->queue) {
                struct vbs_pending* p = s->queue;
                s->queue = p->next;
                pipe_destroy(p->server_end);
                free(p);
            }
            free(s->name);
            free(s);
            break;
        }
    }
    spinlock_unlock(&g_srv_lock);
    handle->pipesrv.srv = NULL;
    return 0;
}

static int pipe_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    memset(attr, 0, sizeof(*attr));
    attr->handle_type = HANDLE_TYPE(handle);
    if (HANDLE_TYPE(handle) == PAL_TYPE_PIPE) {
        attr->nonblocking = handle->pipe.nonblocking;
        spinlock_lock(&handle->pipe.rx->lock);
        attr->pending_size = handle->pipe.rx->len;
        spinlock_unlock(&handle->pipe.rx->lock);
    }
    return 0;
}

static int pipe_attrsetbyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    if (HANDLE_TYPE(handle) == PAL_TYPE_PIPE)
        handle->pipe.nonblocking = attr->nonblocking;
    return 0;
}

/* readiness of a pipe end for _PalStreamsWaitEvents (pal_object.c) */
pal_wait_flags_t vbs_pipe_ready(PAL_HANDLE handle) {
    pal_wait_flags_t r = 0;
    if (HANDLE_TYPE(handle) == PAL_TYPE_PIPESRV) {
        spinlock_lock(&g_srv_lock);
        if (handle->pipesrv.srv && handle->pipesrv.srv->queue)
            r |= PAL_WAIT_READ;
        spinlock_unlock(&g_srv_lock);
        return r;
    }
    if (HANDLE_TYPE(handle) != PAL_TYPE_PIPE)
        return 0;
    struct vbs_chan* rx = handle->pipe.rx;
    struct vbs_chan* tx = handle->pipe.tx;
    spinlock_lock(&rx->lock);
    if (rx->len || rx->closed)
        r |= PAL_WAIT_READ;
    if (rx->closed)
        r |= PAL_WAIT_HANG_UP;
    spinlock_unlock(&rx->lock);
    spinlock_lock(&tx->lock);
    if (tx->len < VBS_CHAN_SIZE && !tx->closed)
        r |= PAL_WAIT_WRITE;
    spinlock_unlock(&tx->lock);
    return r;
}

struct handle_ops g_pipe_ops = {
    .open           = &pipe_open,
    .waitforclient  = &pipe_waitforclient,
    .read           = &pipe_read,
    .write          = &pipe_write,
    .destroy        = &pipe_destroy,
    .delete         = &pipe_delete,
    .attrquerybyhdl = &pipe_attrquerybyhdl,
    .attrsetbyhdl   = &pipe_attrsetbyhdl,
};
