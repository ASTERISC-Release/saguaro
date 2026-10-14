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

extern char __process_data_start[];
extern char __process_data_end[];
static volatile uint64_t g_process_copy_private_check __attribute__((section(".process_data")));
static volatile uint64_t g_process_copy_shared_check;
static int vm_process_copy_runtime(const struct memory_address_space* source,
                                   struct memory_address_space* target, size_t* out_pages);

#define PROC_STREAM_CAPACITY 1984
#define PROC_HANDLE_CAPACITY 8

struct proc_ring {
    atomic_flag lock;
    uint64_t head;
    uint64_t tail;
    bool reader_closed;
    bool writer_closed;
    uint8_t data[PROC_STREAM_CAPACITY];
};

struct proc_handle_queue {
    atomic_flag lock;
    PAL_HANDLE handles[PROC_HANDLE_CAPACITY];
    uint64_t head;
    uint64_t tail;
    bool reader_closed;
    bool writer_closed;
};

struct proc_shared_stream {
    atomic_uint refs;
    atomic_uint side_refs[2];
    uintptr_t handles_frame;
    struct proc_ring directions[2];
};
static_assert(sizeof(struct proc_shared_stream) <= PAGE_SIZE, "process stream must fit one frame");
static_assert(ALIGN_UP(2 * sizeof(struct pal_handle), _Alignof(struct proc_handle_queue)) +
                      2 * sizeof(struct proc_handle_queue) <= PAGE_SIZE,
              "process handle queues must fit the endpoint frame");
static_assert(2 * sizeof(struct pal_handle) <= PAGE_SIZE,
              "process endpoints must fit one handle frame");

static struct proc_shared_stream* proc_stream(PAL_HANDLE handle) {
    return memory_frame_pointer(handle->process.shared_stream_frame);
}

static struct proc_handle_queue* proc_handle_queue(struct proc_shared_stream* shared,
                                                    size_t direction) {
    uintptr_t queues = ALIGN_UP(2 * sizeof(struct pal_handle),
                                _Alignof(struct proc_handle_queue));
    return (struct proc_handle_queue*)((char*)memory_frame_pointer(shared->handles_frame) +
                                       queues) + direction;
}

static void proc_destroy(PAL_HANDLE handle);

static void proc_ring_lock(struct proc_ring* ring) {
    while (atomic_flag_test_and_set_explicit(&ring->lock, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");
}

static void proc_ring_unlock(struct proc_ring* ring) {
    atomic_flag_clear_explicit(&ring->lock, memory_order_release);
}

static void proc_handle_queue_lock(struct proc_handle_queue* queue) {
    while (atomic_flag_test_and_set_explicit(&queue->lock, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");
}

static void proc_handle_queue_unlock(struct proc_handle_queue* queue) {
    atomic_flag_clear_explicit(&queue->lock, memory_order_release);
}

static void proc_handle_queue_close(struct proc_handle_queue* queue, bool reader, bool writer) {
    PAL_HANDLE pending[PROC_HANDLE_CAPACITY];
    size_t pending_count = 0;
    proc_handle_queue_lock(queue);
    if (reader) {
        queue->reader_closed = true;
        while (queue->head != queue->tail) {
            pending[pending_count++] = queue->handles[queue->head++ % PROC_HANDLE_CAPACITY];
        }
    }
    if (writer)
        queue->writer_closed = true;
    proc_handle_queue_unlock(queue);

    for (size_t i = 0; i < pending_count; i++)
        proc_destroy(pending[i]);
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
        proc_handle_queue_close(proc_handle_queue(shared, 1 - handle->process.side), true, false);
        handle->flags &= ~PAL_HANDLE_FD_READABLE;
    }
    if (mode == PAL_DELETE_WRITE || mode == PAL_DELETE_ALL) {
        proc_ring_lock(outgoing);
        outgoing->writer_closed = true;
        proc_ring_unlock(outgoing);
        proc_handle_queue_close(proc_handle_queue(shared, handle->process.side), false, true);
        handle->flags &= ~PAL_HANDLE_FD_WRITABLE;
    }
    return 0;
}

static void proc_destroy(PAL_HANDLE handle) {
    uintptr_t frame = handle->process.shared_stream_frame;
    struct proc_shared_stream* shared = memory_frame_pointer(frame);
    uintptr_t private_frame = handle->private_frame;
    if (atomic_fetch_sub_explicit(&shared->side_refs[handle->process.side], 1,
                                  memory_order_acq_rel) == 1)
        (void)proc_delete(handle, PAL_DELETE_ALL);
    if (atomic_fetch_sub_explicit(&shared->refs, 1, memory_order_acq_rel) == 1) {
        uintptr_t handles_frame = shared->handles_frame;
        (void)memory_frame_free(frame);
        (void)memory_frame_free(handles_frame);
    }
    if (private_frame)
        (void)memory_frame_free(private_frame);
}

int vm_process_send_handle(PAL_HANDLE target_process, PAL_HANDLE cargo) {
    if (!target_process || target_process->hdr.type != PAL_TYPE_PROCESS)
        return -PAL_ERROR_BADHANDLE;
    if (!cargo || cargo->hdr.type != PAL_TYPE_PROCESS)
        return -PAL_ERROR_NOTIMPLEMENTED;

    struct proc_shared_stream* shared = proc_stream(target_process);
    struct proc_handle_queue* queue = proc_handle_queue(shared, target_process->process.side);
    PAL_HANDLE copy = NULL;
    uintptr_t copy_frame = 0;
    int ret = memory_frame_alloc(&copy_frame);
    if (ret < 0)
        return ret;
    copy = memory_frame_pointer(copy_frame);
    memcpy(copy, cargo, sizeof(*copy));
    copy->private_frame = copy_frame;

    struct proc_shared_stream* cargo_shared = proc_stream(copy);
    atomic_fetch_add_explicit(&cargo_shared->refs, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&cargo_shared->side_refs[copy->process.side], 1,
                              memory_order_relaxed);
    while (true) {
        proc_handle_queue_lock(queue);
        if (queue->reader_closed || queue->writer_closed) {
            proc_handle_queue_unlock(queue);
            proc_destroy(copy);
            return -PAL_ERROR_DENIED;
        }
        if (queue->tail - queue->head < PROC_HANDLE_CAPACITY) {
            queue->handles[queue->tail++ % PROC_HANDLE_CAPACITY] = copy;
            proc_handle_queue_unlock(queue);
            return 0;
        }
        proc_handle_queue_unlock(queue);
        if (target_process->process.nonblocking) {
            proc_destroy(copy);
            return -PAL_ERROR_TRYAGAIN;
        }
        _PalThreadYieldExecution();
    }
}

int vm_process_receive_handle(PAL_HANDLE source_process, PAL_HANDLE* out_cargo) {
    if (!source_process || source_process->hdr.type != PAL_TYPE_PROCESS || !out_cargo)
        return -PAL_ERROR_BADHANDLE;

    struct proc_shared_stream* shared = proc_stream(source_process);
    struct proc_handle_queue* queue = proc_handle_queue(shared,
                                                        1 - source_process->process.side);
    while (true) {
        proc_handle_queue_lock(queue);
        if (queue->head != queue->tail) {
            *out_cargo = queue->handles[queue->head++ % PROC_HANDLE_CAPACITY];
            proc_handle_queue_unlock(queue);
            return 0;
        }
        bool eof = queue->writer_closed || queue->reader_closed;
        proc_handle_queue_unlock(queue);
        if (eof)
            return -PAL_ERROR_DENIED;
        if (source_process->process.nonblocking)
            return -PAL_ERROR_TRYAGAIN;
        _PalThreadYieldExecution();
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
    atomic_init(&shared->side_refs[0], 1);
    atomic_init(&shared->side_refs[1], 1);
    shared->handles_frame = handles_frame;
    atomic_flag_clear(&shared->directions[0].lock);
    atomic_flag_clear(&shared->directions[1].lock);
    PAL_HANDLE handles = memory_frame_pointer(handles_frame);
    memset(handles, 0, PAGE_SIZE);
    atomic_flag_clear(&proc_handle_queue(shared, 0)->lock);
    atomic_flag_clear(&proc_handle_queue(shared, 1)->lock);
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
    PAL_HANDLE received = NULL;
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

    ret = vm_process_send_handle(parent, parent);
    if (ret < 0)
        goto out;
    ret = vm_process_receive_handle(child, &received);
    if (ret < 0)
        goto out;
    if (proc_write(received, 0, 1, source) != 1 || proc_read(child, 0, 1, result) != 1 ||
            result[0] != source[0]) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }
    proc_destroy(received);
    received = NULL;

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
    if (received)
        proc_destroy(received);
    proc_destroy(parent);
    proc_destroy(child);
    if (memory_frame_allocated_count() != initial_frames)
        return -PAL_ERROR_DENIED;
    return ret;
}

static int vm_process_copy_runtime(const struct memory_address_space* source,
                                   struct memory_address_space* target, size_t* out_pages) {
    int ret = memory_address_space_clone_from(source, target);
    if (ret < 0)
        return ret;

    size_t copied_pages = ((uintptr_t)__process_data_end - (uintptr_t)__process_data_start) /
                          PAGE_SIZE;
    ret = memory_address_space_copy_private_range(
            (struct memory_address_space*)source, target, (uintptr_t)__process_data_start,
            (uintptr_t)__process_data_end - (uintptr_t)__process_data_start);
    if (ret < 0)
        goto fail;

    /* PAL/kernel memory stays shared across roots. In particular, the scheduler list and lock,
     * stack registry, and allocator state must have one owner while the scheduler switches among
     * threads from different roots. Only process-specific PAL state belongs in .process_data. */

    if (out_pages)
        *out_pages = copied_pages;
    return 0;

fail:
    memory_address_space_destroy(target);
    return ret;
}

int vm_process_runtime_copy_selfcheck(void) {
    const struct memory_address_space* boot = memory_address_space_boot();
    struct memory_address_space copy = {0};
    size_t initial_frames = memory_frame_allocated_count();
    size_t copied_pages = 0;
    const uint64_t private_before = g_process_copy_private_check;
    const uint64_t shared_before = g_process_copy_shared_check;
    const uint64_t private_marker = 0x13579bdf2468ace0UL;
    const uint64_t shared_marker = 0xfedcba9876543210UL;
    bool private_isolated = false;
    bool shared_visible = false;
    size_t manifest_size = g_pal_common_state.raw_manifest_data
            ? strlen(g_pal_common_state.raw_manifest_data) : 0;
    char manifest_prefix[32] = {0};
    bool address_space_active = false;
    if (manifest_size)
        memcpy(manifest_prefix, g_pal_common_state.raw_manifest_data,
               MIN(manifest_size, sizeof(manifest_prefix) - 1));

    g_process_copy_private_check = private_marker;
    g_process_copy_shared_check = shared_before;
    int ret = vm_process_copy_runtime(boot, &copy, &copied_pages);
    if (ret < 0)
        goto restore_state;

    ret = memory_address_space_switch(&copy);
    if (ret < 0)
        goto out;
    address_space_active = true;

    private_isolated = g_process_copy_private_check == private_marker;
    g_process_copy_private_check = 0x0badc0de0ddba11UL;
    g_process_copy_shared_check = shared_marker;

    bool isolated_state = g_pal_public_state.manifest_root &&
                          g_pal_common_state.raw_manifest_data &&
                          !memcmp(g_pal_common_state.raw_manifest_data, manifest_prefix,
                                  MIN(manifest_size, sizeof(manifest_prefix) - 1));
    bool multiprocess_enabled = false;
    if (isolated_state && toml_bool_in(g_pal_public_state.manifest_root,
                                      "sys.experimental__multiprocess", false,
                                      &multiprocess_enabled) < 0)
        isolated_state = false;
    isolated_state &= multiprocess_enabled;

    ret = memory_address_space_switch_root(g_pml4_table_base);
    if (ret < 0)
        goto out;
    address_space_active = false;
    private_isolated &= g_process_copy_private_check == private_marker;
    shared_visible = g_process_copy_shared_check == shared_marker;
    g_process_copy_private_check = private_before;
    g_process_copy_shared_check = shared_before;
    if (!isolated_state) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }
    if (!private_isolated || !shared_visible) {
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    log_always("PAL root state self-check passed (private process state copied, shared PAL state "
               "visible, %zu process-data pages)", copied_pages);

out:
    if (address_space_active) {
        int restore_ret = memory_address_space_switch_root(g_pml4_table_base);
        if (restore_ret < 0)
            return restore_ret;
    }
    memory_address_space_destroy(&copy);
restore_state:
    g_process_copy_private_check = private_before;
    g_process_copy_shared_check = shared_before;
    if (memory_frame_allocated_count() != initial_frames)
        return -PAL_ERROR_DENIED;
    return ret;
}
