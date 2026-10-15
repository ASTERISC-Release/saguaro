/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Internal interface of the vbs PAL proper: the W services and the packed-object table. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pal.h"
#include "pal_internal.h"
#include "pal_vbs_abi.h"

#include "elf/elf.h"

extern const struct pal_vbs_services* g_vbs;
extern const size_t g_page_size;

/* Calls a W service (Windows ARM64 code); x18 always holds the current TEB (pal_vbs_wcall.S). */
uint64_t pal_vbs_wcall(void* fn, uint64_t a0, uint64_t a1, uint64_t a2);

static inline void vbs_console_write(const char* buf, size_t len) {
    pal_vbs_wcall(g_vbs->console_write, (uint64_t)buf, len, 0);
}
static inline uint64_t vbs_time_ns(void) {
    return pal_vbs_wcall(g_vbs->time_ns, 0, 0, 0);
}
static inline int64_t vbs_random(void* buf, size_t len) {
    return (int64_t)pal_vbs_wcall(g_vbs->random, (uint64_t)buf, len, 0);
}
static inline int64_t vbs_protect(uintptr_t addr, size_t size, pal_prot_flags_t prot) {
    return (int64_t)pal_vbs_wcall(g_vbs->protect, addr, size,
                                  prot & (PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC));
}
noreturn void vbs_exit(int64_t code);

/* threads and blocking (W services, version 2) */
static inline int64_t vbs_thread_create(void* entry, void* arg) {
    return (int64_t)pal_vbs_wcall(g_vbs->thread_create, (uint64_t)entry, (uint64_t)arg, 0);
}
noreturn void vbs_thread_exit(void* ctx, int* clear_tid);
/* WaitOnAddress: 0 if woken (or *addr != expected), -1 on timeout; timeout_ns UINT64_MAX = forever */
static inline int64_t vbs_wait(uint32_t* addr, uint32_t expected, uint64_t timeout_ns) {
    return (int64_t)pal_vbs_wcall(g_vbs->wait, (uint64_t)addr, expected, timeout_ns);
}
static inline void vbs_wake(uint32_t* addr, bool all) {
    pal_vbs_wcall(g_vbs->wake, (uint64_t)addr, all, 0);
}

/* the attestation report package for 64 bytes of user data (W service, version 5): its size, or
 * a negative value (W's -HRESULT) */
static inline int64_t vbs_attest(const void* user_data, void* out, size_t out_cap) {
    return (int64_t)pal_vbs_wcall(g_vbs->attest, (uint64_t)user_data, (uint64_t)out, out_cap);
}

/* A host request (files, sockets): `in` (in_len bytes) goes to the host after the header; the
 * host's output (at most out_cap bytes) lands in `out`, its length in *out_len. Returns the host's
 * result (negative Linux errno on failure) or a negative Linux errno of the PAL itself. */
int64_t vbs_hostcall(uint64_t op, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     const void* in, size_t in_len, void* out, size_t out_cap, size_t* out_len);
/* The same with scatter/gather and no staging copy: the input is in_pre (in_pre_len bytes) followed
 * by the bytes of in_iov, as many as fit in a host request (PAL_VBS_HOST_DATA_MAX in all); the
 * output fills out_pre (out_pre_cap bytes), then out_iov; *out_len is the number of bytes stored. */
int64_t vbs_hostcall_v(uint64_t op, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                       const void* in_pre, size_t in_pre_len, const struct iovec* in_iov,
                       size_t in_niov, void* out_pre, size_t out_pre_cap,
                       const struct iovec* out_iov, size_t out_niov, size_t* out_len);

/* host files allowed by the manifest */
struct vbs_trusted_file {
    char* path;
    uint8_t sha256[32];
};
extern struct vbs_trusted_file* g_vbs_trusted_files;
extern size_t g_vbs_trusted_files_cnt;
extern char** g_vbs_allowed_files; /* path prefixes */
extern size_t g_vbs_allowed_files_cnt;
int vbs_init_host_files(void);
const struct vbs_trusted_file* vbs_find_trusted(const char* path);
bool vbs_is_allowed(const char* path);

/* waits shorter than this poll the clock instead of WaitOnAddress, whose timeout is rounded up to
 * the system timer tick */
#define VBS_SHORT_WAIT_NS (2ull * 1000 * 1000)

/* the PAL's per-thread control block */
typedef struct pal_vbs_tcb {
    PAL_TCB common;
    PAL_HANDLE handle;
    void* w_ctx;             /* W's context of this thread (NULL for the first thread) */
    void* stack;             /* PAL stack of a thread created by _PalThreadCreate */
    int (*callback)(void*);
    void* param;
    void* alt_stack;         /* where faults are handled (VBS_ALT_STACK_SIZE bytes) */
} PAL_VBS_TCB;

#define VBS_ALT_STACK_SIZE (64 * 1024)

#if defined(__x86_64__)
/*
 * x86_64 (PLAN-x64.md, X2): the thread's PAL_VBS_TCB lives inside its Windows TEB, at
 * PAL_TCB_GS_OFFSET, so that the %gs-relative TCB accesses of the PAL and the LibOS reach it. The
 * range 0x800-0x1138 of an x64 TEB (Win32 client information and the OpenGL dispatch table) is never
 * used in an enclave. Copies `tcb` there, fixes its self pointer and returns the TEB copy.
 */
#define VBS_TEB_TCB_END 0x1138
static_assert(PAL_TCB_GS_OFFSET + sizeof(PAL_VBS_TCB) <= VBS_TEB_TCB_END,
              "PAL_VBS_TCB does not fit into the unused part of the TEB");
__attribute_no_stack_protector
static inline PAL_VBS_TCB* vbs_arch_place_tcb(PAL_VBS_TCB* tcb) {
    PAL_VBS_TCB* t = (PAL_VBS_TCB*)(pal_vbs_teb() + PAL_TCB_GS_OFFSET);
    if (t != tcb)
        memcpy(t, tcb, sizeof(*t));
    t->common.self = &t->common;
    return t;
}
#endif

/* any stream became ready (pipes): waiters of _PalStreamsWaitEvents and blocking reads wait on it */
extern uint32_t g_vbs_stream_gen;
void vbs_streams_changed(void);

/* packed objects */
const struct pal_vbs_packed_obj* vbs_find_path(const char* path);
const uint8_t* vbs_file_data(const struct pal_vbs_packed_obj* obj);
uint8_t* vbs_image_base(const struct pal_vbs_packed_obj* obj);
/* Strip "file:" and make an absolute normalized path in `out` (size `size`); 0 or negative error. */
int vbs_uri_to_path(const char* uri, char* out, size_t size);

/* where an address range lies */
bool vbs_in_pool(uintptr_t addr, size_t size);
bool vbs_in_pack(uintptr_t addr, size_t size);
/* true if [addr, addr+size) overlaps an executable PT_LOAD of a packed object */
bool vbs_overlaps_packed_code(uintptr_t addr, size_t size);
/* true if [addr, addr+size) lies inside executable PT_LOADs of one packed object */
bool vbs_inside_packed_code(uintptr_t addr, size_t size);

/* the packed-object lookup published to the LibOS (g_pal_public_state.vbs_packed_lookup) */
int vbs_packed_lookup(const char* uri, uintptr_t* out_base, size_t* out_size,
                      uintptr_t* out_syscall_slot);

int vbs_init_memory(void);
extern const struct pal_vbs_packed_obj* g_vbs_libos_obj;

/* readiness of an in-enclave pipe end (pal_pipes.c), for _PalStreamsWaitEvents */
pal_wait_flags_t vbs_pipe_ready(PAL_HANDLE handle);
/* readiness of an in-enclave eventfd (pal_eventfd.c) */
pal_wait_flags_t vbs_eventfd_ready(PAL_HANDLE handle);

/* for pal_rtld.c (PAL_HOST_VBS): where the packed object behind a file handle lies */
int vbs_packed_base(PAL_HANDLE handle, elf_addr_t* out_base, uintptr_t* out_size,
                    uintptr_t* out_syscall_slot);
