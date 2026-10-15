/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Enclave-internal interface of the VBS-enclave PAL (phase 1 skeleton). The PAL exposes a small
 * environment of primitives to a PAL-level test program (test/pal_vbs_selftest.c); keeping the test
 * behind this vtable keeps "the PAL" and "a test that runs on the PAL" separate, as the LibOS will
 * be in phase 2. */

#ifndef PAL_VBS_ENCLAVE_H
#define PAL_VBS_ENCLAVE_H

#include <stdint.h>

#include "pal_vbs_packed.h"

/* Primitives the PAL provides inside the enclave. All widths are fixed (D4). */
typedef struct pal_vbs_env {
    void     (*console_write)(const char* s, uint64_t len); /* OCALL to the host console */
    void*    (*mem_alloc)(uint64_t size);                   /* RW enclave memory (never executable) */
    void     (*mem_free)(void* p, uint64_t size);
    uint64_t (*time_ns)(void);                              /* Unix ns via a host OCALL */
    int64_t  (*random)(void* buf, uint64_t len);            /* BCryptGenRandom; 0 on success */
    uint64_t (*enter_sysv)(void* fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    uint64_t (*read_x18)(void);
    int64_t  (*try_brk_resume)(void);                       /* raise brk under SEH, resume; 0 if ok */
    /* packed objects of this image (pal_vbs_packed.c) */
    uint64_t (*packed_count)(void);
    const char* (*packed_name)(uint64_t i);
    int64_t  (*packed_load)(const char* name, const struct pal_vbs_import* imports,
                            uint64_t nimports, struct pal_vbs_loaded* out, uint64_t* detail);
    void*    (*packed_sym)(const struct pal_vbs_loaded* obj, const char* sym);
    uint64_t (*x18_spin)(uint64_t iterations, uint64_t magic);
    int64_t  (*spin_foreign_stack)(uint64_t iterations); /* spin with SP in VirtualAlloc'd memory */
    int64_t  (*brk_foreign_stack)(void);  /* SEH for a brk with SP there: 0 if caught/resumed */
    int64_t  (*brk_probe)(int widen);     /* same in a function with its own SEH handler */
    uint64_t (*regs_spin)(uint64_t iterations, uint64_t* out_tpidr);
    uint64_t (*get_tpidr)(void);
    void     (*set_tpidr)(uint64_t v);
} pal_vbs_env_t;

/* The PAL-level test. Returns 0 if every step passed, else the 1-based index of the first failure.
 * Writes the number of steps run and passed. */
int64_t pal_vbs_selftest(const pal_vbs_env_t* env, uint64_t* run, uint64_t* passed);

/* boot context (pal_vbs_boundary.S) */
struct pal_vbs_ctx {
    uint64_t regs[23]; /* x19-x30, sp, x18, tpidr_el0, d8-d15 (184 bytes) */
};
int64_t pal_vbs_ctx_save(struct pal_vbs_ctx* c) __attribute__((returns_twice));
void pal_vbs_ctx_resume(struct pal_vbs_ctx* c, int64_t v) __attribute__((noreturn));

/* W services shared by the self-test env and the boot path (pal_vbs_enclave.c) */
void     pal_vbs_w_console_write(const char* s, uint64_t len);
uint64_t pal_vbs_w_time_ns(void);
int64_t  pal_vbs_w_random(void* buf, uint64_t len);
void     pal_vbs_w_log(const char* s);
uint64_t pal_vbs_w_ocall(uint64_t no, uint64_t a0, uint64_t a1, uint64_t a2);
int64_t  pal_vbs_w_register_thread(struct pal_vbs_ocall_args* host_args, uint8_t* host_conbuf,
                                   uint8_t* host_iobuf);
uint8_t* pal_vbs_w_get_iobuf(void);
void     pal_vbs_w_unregister_thread(void);
void     pal_vbs_w_set_exit_state(void* st);
void*    pal_vbs_w_get_exit_state(void);

/* Boot the packed vbs PAL (pal_vbs_boot.c). Returns the exit code of the application, or a
 * negative value if booting failed before the PAL took over. */
struct pal_vbs_shared;
struct _EXCEPTION_POINTERS;
long pal_vbs_report_exception(struct _EXCEPTION_POINTERS* ep, const char* where);
int64_t pal_vbs_boot(const struct pal_vbs_shared* shared);

/* boundary thunks (pal_vbs_boundary.S) */
uint64_t pal_vbs_read_x18(void);
uint64_t pal_vbs_enter_sysv(void* fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
int64_t  pal_vbs_brk0(void);
uint64_t pal_vbs_x18_spin(uint64_t iterations, uint64_t magic);
uint64_t pal_vbs_spin_on_stack(uint64_t iterations, void* stack_top);
int64_t  pal_vbs_brk_on_stack(void* stack_top);
int64_t  pal_vbs_brk_probe(void* stack_top);
uint64_t pal_vbs_regs_spin(uint64_t iterations, uint64_t* out_tpidr);
uint64_t pal_vbs_get_tpidr(void);
uint64_t pal_vbs_tpidr_spin(uint64_t iterations, uint64_t magic, uint64_t* out_first);
void     pal_vbs_set_tpidr(uint64_t v);

#endif /* PAL_VBS_ENCLAVE_H */
