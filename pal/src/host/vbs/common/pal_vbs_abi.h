/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * The host<->enclave ABI boundary of the Windows VBS-enclave PAL (phase 1 skeleton).
 *
 * This header is shared by the enclave side (compiled for aarch64-pc-windows-msvc) and the host
 * side (a Windows ARM64 executable). It defines the two call tables that cross the boundary:
 *
 *   ECALLs  host -> enclave:  the host calls an exported enclave routine with CallEnclave().
 *                             Phase 1 has a single bootstrap ECALL, E_PalMain (see the .def).
 *   OCALLs  enclave -> host:  the enclave calls the host's single `ocall` dispatcher (a host
 *                             function pointer passed in through the shared struct) with
 *                             CallEnclave() from inside the enclave, demultiplexed by an OCALL
 *                             number. This mirrors the SGX PAL's OCALL table.
 *
 * Boundary conventions (decided in Documentation/devel/vbs-enclave/PLAN.md, D4; measured in
 * phase 0):
 *   - EVERY value that crosses the boundary is a fixed-width type (uint64_t / int64_t / pointers).
 *     Never `long`: Windows ARM64 is LLP64 (long = 32 bits), Linux AArch64 is LP64 (long = 64).
 *   - x18 is the enclave TEB and must be preserved across any call into Linux System V/AAPCS64
 *     code and back (phase 2 glibc/LibOS). The enclave boundary thunk saves/restores it.
 *   - The host cannot read enclave memory (phase 0), so anything the host must see is copied OUT
 *     of the enclave into the host-owned shared struct with EnclaveCopyOutOfEnclave() first.
 */

#ifndef PAL_VBS_ABI_H
#define PAL_VBS_ABI_H

#include <stdint.h>

/* OCALL numbers (enclave -> host). Keep in sync on both sides. */
enum pal_vbs_ocall {
    PAL_VBS_OCALL_CONSOLE_WRITE = 0, /* a0 = byte count already placed in shared->conbuf; prints it */
    PAL_VBS_OCALL_GET_TIME_NS   = 1, /* returns Unix time in nanoseconds (host wall clock) */
    PAL_VBS_OCALL_EXIT          = 2, /* a0 = exit code; records it, returns (host tears the enclave down) */
    PAL_VBS_OCALL_MAX
};

#define PAL_VBS_CONBUF_SIZE 4096u /* host-owned bounce buffer for console OCALLs */

/*
 * The host OCALL dispatcher. The host passes its address into the enclave in `pal_vbs_shared.ocall`.
 * The enclave invokes it with CallEnclave(ocall, &shared->ocall_args, TRUE, &ret). All fields are
 * fixed-width. The dispatcher runs on the host thread that made the originating ECALL.
 */
typedef struct pal_vbs_ocall_args {
    uint64_t ocall_no; /* one of enum pal_vbs_ocall */
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t retval;   /* filled by the host */
} pal_vbs_ocall_args_t;

/*
 * The bootstrap struct. It lives in HOST memory; the host passes its address as the single
 * parameter of the E_PalMain ECALL. The enclave copies it in with EnclaveCopyIntoEnclave(), uses
 * `ocall` for all host services, and copies console bytes out into `conbuf` before each console
 * OCALL. `result`/`log_len` are written back on exit.
 */
typedef struct pal_vbs_shared {
    uint64_t abi_version;          /* PAL_VBS_ABI_VERSION; host and enclave must agree */
    uint64_t thread_count;         /* N passed to InitializeEnclave (ENCLAVE_INIT_INFO_VBS) */
    uint64_t enclave_base;         /* host's view of the enclave base (for logging) */
    uint64_t enclave_size;         /* image-configured size */

    /* OCALL path */
    void*    ocall;                /* host dispatcher, type pal_vbs_ocall_fn */
    pal_vbs_ocall_args_t ocall_args;
    uint8_t  conbuf[PAL_VBS_CONBUF_SIZE];

    /* results written back by the enclave before returning from E_PalMain */
    int64_t  result;               /* 0 = all self-tests passed, else first failing step */
    uint64_t selftests_run;
    uint64_t selftests_passed;
} pal_vbs_shared_t;

typedef uint64_t (*pal_vbs_ocall_fn)(pal_vbs_ocall_args_t* args);

#define PAL_VBS_ABI_VERSION 1u

#endif /* PAL_VBS_ABI_H */
