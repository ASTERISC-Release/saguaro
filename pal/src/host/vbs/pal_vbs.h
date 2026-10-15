/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

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

/* the PAL's per-thread control block */
typedef struct pal_vbs_tcb {
    PAL_TCB common;
    PAL_HANDLE handle;
    void* w_ctx;             /* W's context of this thread (NULL for the first thread) */
    void* stack;             /* PAL stack of a thread created by _PalThreadCreate */
    int (*callback)(void*);
    void* param;
} PAL_VBS_TCB;

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

/* for pal_rtld.c (PAL_HOST_VBS): where the packed object behind a file handle lies */
int vbs_packed_base(PAL_HANDLE handle, elf_addr_t* out_base, uintptr_t* out_size,
                    uintptr_t* out_syscall_slot);
