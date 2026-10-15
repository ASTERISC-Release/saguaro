/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

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
} pal_vbs_env_t;

/* The PAL-level test. Returns 0 if every step passed, else the 1-based index of the first failure.
 * Writes the number of steps run and passed. */
int64_t pal_vbs_selftest(const pal_vbs_env_t* env, uint64_t* run, uint64_t* passed);

/* boundary thunks (pal_vbs_boundary.S) */
uint64_t pal_vbs_read_x18(void);
uint64_t pal_vbs_enter_sysv(void* fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
int64_t  pal_vbs_brk0(void);

#endif /* PAL_VBS_ENCLAVE_H */
