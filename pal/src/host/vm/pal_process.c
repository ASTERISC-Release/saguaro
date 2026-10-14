/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/* VM PAL process handles use a frame shared by all address spaces. Process creation itself is
 * enabled only after child startup and process lifetime management are implemented. */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_interrupts.h"
#include "kernel_memory.h"

#define PROC_STREAM_CAPACITY 1984

struct proc_ring {
    atomic_flag lock;
    uint64_t head;
    uint64_t tail;
    bool reader_closed;
    bool writer_closed;
    uint8_t data[PROC_STREAM_CAPACITY];
};

struct proc_shared_stream {
    atomic_uint refs;
    uintptr_t handles_frame;
    struct proc_ring directions[2];
};
static_assert(sizeof(struct proc_shared_stream) <= PAGE_SIZE, "process stream must fit one frame");
static_assert(2 * sizeof(struct pal_handle) <= PAGE_SIZE,
              "process endpoints must fit one handle frame");

static struct proc_shared_stream* proc_stream(PAL_HANDLE handle) {
    return memory_frame_pointer(handle->process.shared_stream_frame);
}

static void proc_ring_lock(struct proc_ring* ring) {
    while (atomic_flag_test_and_set_explicit(&ring->lock, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");
}

static void proc_ring_unlock(struct proc_ring* ring) {
    atomic_flag_clear_explicit(&ring->lock, memory_order_release);
}

static size_t proc_ring_copy_out(struct proc_ring* ring, void* buffer, size_t count) {
    size_t available = ring->tail - ring->head;
    size_t bytes = MIN(count, available);
    size_t first = MIN(bytes, PROC_STREAM_CAPACITY - (ring->head % PROC_STREAM_CAPACITY));
    memcpy(buffer, &ring->data[ring->head % PROC_STREAM_CAPACITY], first);
    memcpy((char*)buffer + first, ring->data, bytes - first);
    ring->head += bytes;
    return bytes;
}

static size_t proc_ring_copy_in(struct proc_ring* ring, const void* buffer, size_t count) {
    size_t used = ring->tail - ring->head;
    size_t available = PROC_STREAM_CAPACITY - used;
    size_t bytes = MIN(count, available);
    size_t first = MIN(bytes, PROC_STREAM_CAPACITY - (ring->tail % PROC_STREAM_CAPACITY));
    memcpy(&ring->data[ring->tail % PROC_STREAM_CAPACITY], buffer, first);
    memcpy(ring->data, (const char*)buffer + first, bytes - first);
    ring->tail += bytes;
    return bytes;
}

static int64_t proc_read(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buffer) {
    if (offset || count > SIZE_MAX)
        return -PAL_ERROR_INVAL;
    if (!(handle->flags & PAL_HANDLE_FD_READABLE))
        return -PAL_ERROR_DENIED;
    if (!count)
        return 0;

    struct proc_shared_stream* shared = proc_stream(handle);
    struct proc_ring* ring = &shared->directions[1 - handle->process.side];
    while (true) {
        proc_ring_lock(ring);
        size_t bytes = proc_ring_copy_out(ring, buffer, count);
        bool eof = ring->writer_closed && ring->head == ring->tail;
        proc_ring_unlock(ring);
        if (bytes || eof)
            return bytes;
        if (handle->process.nonblocking)
            return -PAL_ERROR_TRYAGAIN;
        _PalThreadYieldExecution();
    }
}

static int64_t proc_write(PAL_HANDLE handle, uint64_t offset, uint64_t count,
                          const void* buffer) {
    if (offset || count > SIZE_MAX)
        return -PAL_ERROR_INVAL;
    if (!(handle->flags & PAL_HANDLE_FD_WRITABLE))
        return -PAL_ERROR_DENIED;
    if (!count)
        return 0;

    struct proc_shared_stream* shared = proc_stream(handle);
    struct proc_ring* ring = &shared->directions[handle->process.side];
    while (true) {
        proc_ring_lock(ring);
        bool broken = ring->reader_closed;
        size_t bytes = broken ? 0 : proc_ring_copy_in(ring, buffer, count);
        proc_ring_unlock(ring);
        if (broken)
            return -PAL_ERROR_DENIED;
        if (bytes)
            return bytes;
        if (handle->process.nonblocking)
            return -PAL_ERROR_TRYAGAIN;
        _PalThreadYieldExecution();
    }
}

static int proc_delete(PAL_HANDLE handle, enum pal_delete_mode mode) {
    struct proc_shared_stream* shared = proc_stream(handle);
    struct proc_ring* incoming = &shared->directions[1 - handle->process.side];
    struct proc_ring* outgoing = &shared->directions[handle->process.side];

    if (mode != PAL_DELETE_READ && mode != PAL_DELETE_WRITE && mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;
    if (mode == PAL_DELETE_READ || mode == PAL_DELETE_ALL) {
        proc_ring_lock(incoming);
        incoming->reader_closed = true;
        proc_ring_unlock(incoming);
        handle->flags &= ~PAL_HANDLE_FD_READABLE;
    }
    if (mode == PAL_DELETE_WRITE || mode == PAL_DELETE_ALL) {
        proc_ring_lock(outgoing);
        outgoing->writer_closed = true;
        proc_ring_unlock(outgoing);
        handle->flags &= ~PAL_HANDLE_FD_WRITABLE;
    }
    return 0;
}

static void proc_destroy(PAL_HANDLE handle) {
    (void)proc_delete(handle, PAL_DELETE_ALL);
    uintptr_t frame = handle->process.shared_stream_frame;
    struct proc_shared_stream* shared = memory_frame_pointer(frame);
    if (atomic_fetch_sub_explicit(&shared->refs, 1, memory_order_acq_rel) == 1) {
        uintptr_t handles_frame = shared->handles_frame;
        (void)memory_frame_free(frame);
        (void)memory_frame_free(handles_frame);
    }
}

static int proc_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    struct proc_ring* incoming = &proc_stream(handle)->directions[1 - handle->process.side];
    proc_ring_lock(incoming);
    attr->handle_type = handle->hdr.type;
    attr->nonblocking = handle->process.nonblocking;
    attr->pending_size = incoming->tail - incoming->head;
    proc_ring_unlock(incoming);
    return 0;
}

static int proc_attrsetbyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    handle->process.nonblocking = attr->nonblocking;
    return 0;
}

struct handle_ops g_proc_ops = {
    .read           = &proc_read,
    .write          = &proc_write,
    .destroy        = &proc_destroy,
    .delete         = &proc_delete,
    .attrquerybyhdl = &proc_attrquerybyhdl,
    .attrsetbyhdl   = &proc_attrsetbyhdl,
};

static int proc_create_pair(PAL_HANDLE* parent, PAL_HANDLE* child) {
    uintptr_t frame, handles_frame;
    int ret = memory_frame_alloc(&frame);
    if (ret < 0)
        return ret;
    ret = memory_frame_alloc(&handles_frame);
    if (ret < 0) {
        memory_frame_free(frame);
        return ret;
    }

    struct proc_shared_stream* shared = memory_frame_pointer(frame);
    memset(shared, 0, PAGE_SIZE);
    atomic_init(&shared->refs, 2);
    shared->handles_frame = handles_frame;
    atomic_flag_clear(&shared->directions[0].lock);
    atomic_flag_clear(&shared->directions[1].lock);
    PAL_HANDLE handles = memory_frame_pointer(handles_frame);
    memset(handles, 0, PAGE_SIZE);
    for (size_t i = 0; i < 2; i++) {
        init_handle_hdr(&handles[i], PAL_TYPE_PROCESS);
        handles[i].flags = PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;
        handles[i].process.shared_stream_frame = frame;
        handles[i].process.side = i;
    }
    *parent = &handles[0];
    *child = &handles[1];
    return 0;
}

noreturn void _PalProcessExit(int exitcode) {
    log_always("[ VM exited with code %d ]", exitcode);
    triple_fault();
}

int _PalProcessCreate(const char** args, uintptr_t (*reserved_mem_ranges)[2],
                      size_t reserved_mem_ranges_len, PAL_HANDLE* out_handle) {
    __UNUSED(args);
    __UNUSED(reserved_mem_ranges);
    __UNUSED(reserved_mem_ranges_len);
    __UNUSED(out_handle);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int vm_process_stream_selfcheck(void) {
    size_t initial_frames = memory_frame_allocated_count();
    PAL_HANDLE parent = NULL;
    PAL_HANDLE child = NULL;
    int ret = proc_create_pair(&parent, &child);
    if (ret < 0)
        return ret;

    uint8_t source[PROC_STREAM_CAPACITY * 2];
    uint8_t result[PROC_STREAM_CAPACITY * 2];
    for (size_t i = 0; i < sizeof(source); i++)
        source[i] = i & 0xff;

    int64_t bytes = proc_write(parent, 0, sizeof(source), source);
    if (bytes != PROC_STREAM_CAPACITY) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    PAL_STREAM_ATTR attr = {0};
    ret = proc_attrquerybyhdl(child, &attr);
    if (ret < 0 || attr.pending_size != PROC_STREAM_CAPACITY) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    bytes = proc_read(child, 0, PROC_STREAM_CAPACITY / 2, result);
    if (bytes != PROC_STREAM_CAPACITY / 2 ||
            memcmp(result, source, PROC_STREAM_CAPACITY / 2)) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    bytes = proc_write(parent, 0, PROC_STREAM_CAPACITY / 2, source + PROC_STREAM_CAPACITY);
    if (bytes != PROC_STREAM_CAPACITY / 2)
        ret = -PAL_ERROR_DENIED;
    if (ret < 0)
        goto out;

    bytes = proc_read(child, 0, sizeof(result), result);
    if (bytes != PROC_STREAM_CAPACITY ||
            memcmp(result, source + PROC_STREAM_CAPACITY / 2, PROC_STREAM_CAPACITY / 2) ||
            memcmp(result + PROC_STREAM_CAPACITY / 2, source + PROC_STREAM_CAPACITY,
                   PROC_STREAM_CAPACITY / 2)) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    attr.nonblocking = true;
    ret = proc_attrsetbyhdl(parent, &attr);
    if (ret < 0)
        goto out;
    if (proc_read(parent, 0, 1, result) != -PAL_ERROR_TRYAGAIN) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    ret = proc_delete(child, PAL_DELETE_WRITE);
    if (ret < 0 || proc_read(parent, 0, 1, result) != 0) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }
    ret = proc_delete(parent, PAL_DELETE_READ);
    if (ret < 0 || proc_write(child, 0, 1, source) != -PAL_ERROR_DENIED) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }
    ret = 0;

out:
    proc_destroy(parent);
    proc_destroy(child);
    if (memory_frame_allocated_count() != initial_frames)
        return -PAL_ERROR_DENIED;
    return ret;
}
