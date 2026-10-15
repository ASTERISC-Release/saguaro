/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains definitions of functions, variables and data structures for internal uses.
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "log.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_topology.h"
#include "toml.h"

#ifndef IN_PAL
#error "pal_internal.h can only be included in PAL"
#endif

/*
 * Part of PAL state which is common to all PALs (trusted part, in case of TEE PALs).
 * Most of it is actually very Linux-specific, we'll need to refactor it if we ever add a non-Linux
 * PAL.
 */
struct pal_common_state {
    PAL_HANDLE parent_process;
    const char* raw_manifest_data;
};
extern struct pal_common_state g_pal_common_state;
extern struct pal_public_state g_pal_public_state;

/* handle_ops is the operators provided for each handler type. They are mostly used by
 * stream-related PAL calls, but can also be used by some others in special ways. */
struct handle_ops {
    /* 'open' is used by PalStreamOpen. 'handle' is a preallocated handle, 'type' will be a
     * normalized prefix, 'uri' is the remaining string of uri. access, share, create, and options
     * follow the same flags defined for PalStreamOpen in pal.h. */
    int (*open)(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                pal_share_flags_t share, enum pal_create_mode create, pal_stream_options_t options);

    /* 'read' and 'write' is used by PalStreamRead and PalStreamWrite, so they have exactly same
     * prototype as them. */
    int64_t (*read)(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buffer);
    int64_t (*write)(PAL_HANDLE handle, uint64_t offset, uint64_t count, const void* buffer);

    /* 'delete' is used by PalStreamDelete: for files and dirs it corresponds to unlinking, for
     * sockets it corresponds to shutting down a socket connection. */
    int (*delete)(PAL_HANDLE handle, enum pal_delete_mode delete_mode);

    /* 'destroy' is used by PalObjectDestroy: it closes all associated resources on the host (e.g.
     * closes the host FD), frees all sub-objects of the PAL handle (e.g. a filename string) and
     * finally frees the PAL handle object itself */
    void (*destroy)(PAL_HANDLE handle);

    /*
     * 'map' and 'unmap' will map or unmap the handle into memory space, it's not necessary mapped
     * by mmap, so unmap also needs 'handle' to deal with special cases.
     *
     * Common PAL code will ensure that address, offset, and size are page-aligned. 'address'
     * should not be NULL.
     */
    int (*map)(PAL_HANDLE handle, void* address, pal_prot_flags_t prot, uint64_t offset,
               uint64_t size);

    /* 'setlength' is used by PalStreamFlush. It truncates the stream to certain size. */
    int (*setlength)(PAL_HANDLE handle, uint64_t length);

    /* 'flush' is used by PalStreamFlush. It syncs the stream to the device */
    int (*flush)(PAL_HANDLE handle);

    /* 'waitforclient' is used by PalStreamWaitforClient. It accepts an connection */
    int (*waitforclient)(PAL_HANDLE server, PAL_HANDLE* client, pal_stream_options_t options);

    /* 'attrquery' is used by PalStreamAttributesQuery. It queries the attributes of a stream */
    int (*attrquery)(const char* type, const char* uri, PAL_STREAM_ATTR* attr);

    /* 'attrquerybyhdl' is used by PalStreamAttributesQueryByHandle. It queries the attributes of
     * a stream handle */
    int (*attrquerybyhdl)(PAL_HANDLE handle, PAL_STREAM_ATTR* attr);

    /* 'attrsetbyhdl' is used by PalStreamAttributesSetByHandle. It queries the attributes of
     * a stream handle */
    int (*attrsetbyhdl)(PAL_HANDLE handle, PAL_STREAM_ATTR* attr);

    /* 'rename' is used to change name of a stream, or reset its share option */
    int (*rename)(PAL_HANDLE handle, const char* type, const char* uri);

    /* Path-based file creation operations. These are used only by file handles. */
    int (*symlink)(const char* target, const char* uri);
    int (*link)(const char* old_uri, const char* new_uri);
    int (*unlink_path)(const char* uri);
    int (*rename_path)(const char* old_uri, const char* new_uri);
};

extern const struct handle_ops* g_pal_handle_ops[];

static inline const struct handle_ops* HANDLE_OPS(PAL_HANDLE handle) {
    int _type = handle->hdr.type;
    if (_type < 0 || _type >= PAL_HANDLE_TYPE_BOUND)
        return NULL;
    if (handle->hdr.ops) {
        /* TODO: remove this hack or (preferably) add this for every type of handle. */
        return handle->hdr.ops;
    }
    return g_pal_handle_ops[_type];
}

/* We allow dynamic size handle allocation. Here is some macro to help deciding the actual size of
 * the handle */
extern PAL_HANDLE _h;
#define HANDLE_SIZE(type) (sizeof(*_h))

static inline size_t handle_size(PAL_HANDLE handle) {
    return sizeof(*handle);
}

struct socket_ops {
    int (*bind)(PAL_HANDLE handle, struct pal_socket_addr* addr);
    int (*listen)(PAL_HANDLE handle, unsigned int backlog);
    int (*accept)(PAL_HANDLE handle, pal_stream_options_t options, PAL_HANDLE* out_client,
                  struct pal_socket_addr* out_client_addr, struct pal_socket_addr* out_local_addr);
    int (*connect)(PAL_HANDLE handle, struct pal_socket_addr* addr,
                   struct pal_socket_addr* out_local_addr, bool* out_inprogress);
    int (*send)(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_size,
                struct pal_socket_addr* addr, bool force_nonblocking);
    int (*recv)(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_size,
                struct pal_socket_addr* addr, bool force_nonblocking);
    int (*register_shmem)(PAL_HANDLE handle, void *region);

};

/*
 * failure notify. The rountine is called whenever a PAL call return error code. As the current
 * design of PAL does not return error code directly, we rely on PalAsynchronousEventUpcall to
 * handle PAL call error. If the user does not set up a upcall, the error code will be ignored.
 * Ignoring PAL error code can be a possible optimization for LibOS.
 */
void notify_failure(unsigned long error);

#define IS_ALLOC_ALIGNED(addr)     IS_ALIGNED_POW2(addr, g_pal_public_state.alloc_align)
#define IS_ALLOC_ALIGNED_PTR(addr) IS_ALIGNED_PTR_POW2(addr, g_pal_public_state.alloc_align)
#define ALLOC_ALIGN_UP(addr)       ALIGN_UP_POW2(addr, g_pal_public_state.alloc_align)
#define ALLOC_ALIGN_UP_PTR(addr)   ALIGN_UP_PTR_POW2(addr, g_pal_public_state.alloc_align)
#define ALLOC_ALIGN_DOWN(addr)     ALIGN_DOWN_POW2(addr, g_pal_public_state.alloc_align)
#define ALLOC_ALIGN_DOWN_PTR(addr) ALIGN_DOWN_PTR_POW2(addr, g_pal_public_state.alloc_align)

/*!
 * \brief Main initialization function.
 *
 * \param instance_id     Current instance ID.
 * \param exec_uri        Executable URI.
 * \param parent_process  Parent process if it's a child.
 * \param first_thread    First thread handle.
 * \param arguments       Application arguments.
 * \param environments    Environment variables.
 * \param post_callback   Callback into host-specific loader, useful for post-initialization actions
 *                        like additional logging. Can be NULL.
 *
 * This function must be called by the host-specific loader.
 */
noreturn void pal_main(uint64_t instance_id, PAL_HANDLE parent_process, PAL_HANDLE first_thread,
                       const char** arguments, const char** environments,
                       void (*post_callback)(void));

/* For initialization */

unsigned long _PalMemoryQuota(void);
void PalResetMemoryBookkeepingUpcallsForChild(void);
int _PalDeviceIoControl(PAL_HANDLE handle, uint32_t cmd, unsigned long arg, int* out_ret);
/* Host passthrough (pal_host.c has weak defaults; the Linux PAL implements them) */
int _PalHostReadlink(const char* uri, char* buf, size_t* size);
int _PalHostFdWrap(int host_fd, PAL_HANDLE* out_handle);
int _PalHostFdGet(PAL_HANDLE handle, int* out_host_fd);
int _PalHostClockMonotonic(uint64_t* out_nsec);
int _PalHostUnixConnect(const char* path, pal_stream_options_t options, PAL_HANDLE* out_handle);
int _PalHostSendmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, const int* host_fds,
                    size_t host_fds_cnt, bool nonblocking, size_t* out_size);
int _PalHostRecvmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, int* host_fds,
                    size_t* host_fds_cnt, bool nonblocking, size_t* out_size);
// Returns 0 on success, negative PAL code on failure
int _PalGetCPUInfo(struct pal_cpu_info* info);

/* PalStream calls */
int _PalStreamOpen(PAL_HANDLE* handle, const char* uri, enum pal_access access,
                   pal_share_flags_t share, enum pal_create_mode create,
                   pal_stream_options_t options);
int _PalStreamDelete(PAL_HANDLE handle, enum pal_delete_mode delete_mode);
int64_t _PalStreamRead(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buf);
int64_t _PalStreamWrite(PAL_HANDLE handle, uint64_t offset, uint64_t count, const void* buf);
int _PalStreamAttributesQuery(const char* uri, PAL_STREAM_ATTR* attr);
int _PalStreamAttributesQueryByHandle(PAL_HANDLE hdl, PAL_STREAM_ATTR* attr);
int _PalStreamMap(PAL_HANDLE handle, void* addr, pal_prot_flags_t prot, uint64_t offset,
                  uint64_t size);
int _PalStreamSetLength(PAL_HANDLE handle, uint64_t length);
int _PalStreamFlush(PAL_HANDLE handle);
int _PalSendHandle(PAL_HANDLE target_process, PAL_HANDLE cargo);
int _PalReceiveHandle(PAL_HANDLE source_process, PAL_HANDLE* out_cargo);

int _PalSocketCreate(enum pal_socket_domain domain, enum pal_socket_type type,
                     pal_stream_options_t options, PAL_HANDLE* out_handle);
int _PalSocketBind(PAL_HANDLE handle, struct pal_socket_addr* addr);
int _PalSocketListen(PAL_HANDLE handle, unsigned int backlog);
int _PalSocketAccept(PAL_HANDLE handle, pal_stream_options_t options, PAL_HANDLE* out_client,
                     struct pal_socket_addr* out_client_addr,
                     struct pal_socket_addr* out_local_addr);
int _PalSocketConnect(PAL_HANDLE handle, struct pal_socket_addr* addr,
                      struct pal_socket_addr* out_local_addr, bool* out_inprogress);
int _PalSocketSend(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_size,
                   struct pal_socket_addr* addr, bool force_nonblocking);
int _PalSocketRecv(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_total_size,
                   struct pal_socket_addr* addr, bool force_nonblocking);
int _PalSocketRegisterShmem(PAL_HANDLE handle, void *region);
                   

/* PalProcess and PalThread calls */
int _PalThreadCreate(PAL_HANDLE* handle, int (*callback)(void*), void* param);
noreturn void _PalThreadExit(int* clear_child_tid);
void _PalThreadYieldExecution(void);
int _PalThreadResume(PAL_HANDLE thread_handle);
int _PalProcessCreate(const char** args, uintptr_t (*reserved_mem_ranges)[2],
                      size_t reserved_mem_ranges_len, PAL_HANDLE* out_handle);
noreturn void _PalProcessExit(int exit_code);
int _PalThreadSetCpuAffinity(PAL_HANDLE thread, unsigned long* cpu_mask, size_t cpu_mask_len);
int _PalThreadGetCpuAffinity(PAL_HANDLE thread, unsigned long* cpu_mask, size_t cpu_mask_len);

/* PalEvent calls */
int _PalEventCreate(PAL_HANDLE* handle_ptr, bool init_signaled, bool auto_clear);
void _PalEventSet(PAL_HANDLE handle);
void _PalEventClear(PAL_HANDLE handle);
int _PalEventWait(PAL_HANDLE handle, uint64_t* timeout_us);

/* PalVirtualMemory calls */
int _PalVirtualMemoryAlloc(void* addr, uint64_t size, pal_prot_flags_t prot);
int _PalVirtualMemoryFree(void* addr, uint64_t size);
int _PalVirtualMemoryProtect(void* addr, uint64_t size, pal_prot_flags_t prot);

/* PalObject calls */
void _PalObjectDestroy(PAL_HANDLE object_handle);
int _PalStreamsWaitEvents(size_t count, PAL_HANDLE* handle_array, pal_wait_flags_t* events,
                          pal_wait_flags_t* ret_events, uint64_t* timeout_us);

/* PalException calls & structures */
pal_event_handler_t _PalGetExceptionHandler(enum pal_event event);

int _PalSystemTimeQuery(uint64_t* out_usec);

/*
 * Cryptographically secure random.
 * 0 on success, negative on failure.
 */
int _PalRandomBitsRead(void* buffer, size_t size);

#if defined(__aarch64__) && defined(PAL_HOST_VM)
/* the vm kernel is built -mgeneral-regs-only: this returns the IEEE-754 bit pattern of the
 * double that the other flavors return in their FP return register (see vm meson.build) */
uint64_t _PalGetBogomips(void);
#else
double _PalGetBogomips(void);
#endif
int _PalSegmentBaseGet(enum pal_segment_reg reg, uintptr_t* addr);
int _PalSegmentBaseSet(enum pal_segment_reg reg, uintptr_t addr);
int _PalCpuIdRetrieve(uint32_t leaf, uint32_t subleaf, uint32_t values[4]);
int _PalAttestationReport(const void* user_report_data, size_t* user_report_data_size,
                          void* target_info, size_t* target_info_size, void* report,
                          size_t* report_size);
int _PalAttestationQuote(const void* user_report_data, size_t user_report_data_size, void* quote,
                         size_t* quote_size);
int _PalGetSpecialKey(const char* name, void* key, size_t* key_size);

#define INIT_FAIL(msg, ...)                                                              \
    do {                                                                                 \
        log_error("PAL failed " msg, ##__VA_ARGS__);                                     \
        _PalProcessExit(1);                                                              \
    } while (0)

#define INIT_FAIL_MANIFEST(reason)                                      \
    do {                                                                \
        log_error("PAL failed at parsing the manifest: %s", reason);    \
        _PalProcessExit(1);                                             \
    } while (0)

/*!
 * \brief Get the next reserved memory range.
 *
 * \param      last_range_start      The previous reserved memory range start.
 * \param[out] out_next_range_start  Contains new range start at return.
 * \param[out] out_next_range_end    Contains new range end at return.
 *
 * The next (returned) reserved memory range will be strictly below (at lower address) than
 * the previous one.
 */
void pal_read_next_reserved_range(uintptr_t last_range_start, uintptr_t* out_next_range_start,
                                  uintptr_t* out_next_range_end);

/*!
 * \brief Add initial memory range.
 *
 * \param addr     Address of the start of the range.
 * \param size     Size of the range.
 * \param prot     Memory protection of the range.
 * \param is_user_reserved Whether the range is reserved for a later user mapping.
 * \param comment  Comment associated with this memory range.
 *
 * Caller must make sure that the new range does not overlap any previously added range.
 */
int pal_add_initial_range(uintptr_t addr, size_t size, pal_prot_flags_t prot,
                          bool is_user_reserved, const char* comment);
void pal_clamp_initial_ranges_to_window(void);
int pal_internal_memory_bkeep(size_t size, uintptr_t* out_addr);
int pal_internal_memory_alloc(size_t size, void** out_addr);
int pal_internal_memory_free(void* addr, size_t size);
void pal_disable_early_memory_bookkeeping(void);

/* The x86_64 VM PAL copies the `.process_data` section privately into each process address space
 * (see pal/src/host/vm/pal.lds); every other PAL keeps these variables in ordinary sections. */
#if defined(PAL_HOST_VM) && defined(__x86_64__)
#define PAL_PROCESS_DATA      __attribute__((section(".process_data")))
#define PAL_PROCESS_DATA_PAGE __attribute__((section(".process_data"), aligned(4096)))
#else
#define PAL_PROCESS_DATA
#define PAL_PROCESS_DATA_PAGE
#endif

void init_slab_mgr(void);
#if defined(PAL_HOST_VM) && defined(__x86_64__)
int pal_enable_shared_frame_slab_allocator(void);
/* Drops one reference of a handle shared with another process; true if others still hold it. */
bool vm_handle_release_share(PAL_HANDLE handle);
/* Per-process record of the handles given to the LibOS, destroyed when the process exits. */
void vm_track_handle(PAL_HANDLE handle);
void vm_untrack_handle(PAL_HANDLE handle);
#define PAL_TRACK_HANDLE(ret, handle)              \
    do {                                           \
        if ((ret) >= 0 && (handle))                \
            vm_track_handle(handle);               \
    } while (0)
#else
#define PAL_TRACK_HANDLE(ret, handle) do {} while (0)
#endif
void* malloc(size_t size);
void* calloc(size_t num, size_t size);
void free(void* mem);

int _PalInitDebugStream(const char* path);
int _PalDebugLog(const void* buf, size_t size);

// TODO(mkow): We should make it cross-object-inlinable, ideally by enabling LTO, less ideally by
// pasting it here and making `inline`, but our current linker scripts prevent both.
void pal_log(int level, const char* file, const char* func, uint64_t line,
             const char* fmt, ...) __attribute__((format(printf, 5, 6)));

#define PAL_LOG_DEFAULT_LEVEL  LOG_LEVEL_ERROR
#define PAL_LOG_DEFAULT_FD     2

const char* pal_event_name(enum pal_event event);

#define uthash_fatal(msg)                      \
    do {                                       \
        log_error("uthash error: %s", msg);    \
        _PalProcessExit(1);                    \
    } while (0)
#include "uthash.h"
