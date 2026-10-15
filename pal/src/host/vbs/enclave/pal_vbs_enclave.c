/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Enclave side of the Windows VBS-enclave PAL (phase 1 skeleton).
 *
 * This is a signed ARM64 enclave DLL. It is NOT yet wired to Gramine's common PAL sources or the
 * LibOS (that needs the phase-2 packer and loader changes); phase 1 proves the host<->enclave
 * boundary: the PAL entry (E_PalMain ECALL), the Windows<->System-V ABI thunk with x18 preserved,
 * an SEH exception hook, console/time/exit via OCALLs, an RW memory allocator and random.
 *
 * The enclave configuration and DLL skeleton follow Microsoft's VbsEnclave sample (MIT); see
 * test/vbs-enclave/step0-sample/.
 */

#include <winenclave.h>
#include <bcrypt.h>
#include <stdint.h>
#include <string.h>

#include "pal_vbs_abi.h"
#include "pal_vbs_enclave.h"

#ifndef PAL_VBS_POLICY
#define PAL_VBS_POLICY IMAGE_ENCLAVE_POLICY_DEBUGGABLE /* DO NOT SHIP DEBUGGABLE ENCLAVES */
#endif
#ifndef PAL_VBS_SIZE
#define PAL_VBS_SIZE 0x10000000 /* 256 MiB */
#endif
#ifndef PAL_VBS_THREADS
#define PAL_VBS_THREADS 4
#endif

const IMAGE_ENCLAVE_CONFIG __enclave_config = {
    sizeof(IMAGE_ENCLAVE_CONFIG),
    IMAGE_ENCLAVE_MINIMUM_CONFIG_SIZE,
    PAL_VBS_POLICY,
    0, 0, 0,
    { 0xFE, 0xFE },    /* family id */
    { 0x01, 0x04 },    /* image id (0x04: the PAL skeleton with packed objects, phase 2) */
    0,                 /* version */
    0,                 /* SVN */
    PAL_VBS_SIZE,
    PAL_VBS_THREADS,
    IMAGE_ENCLAVE_FLAG_PRIMARY_IMAGE
};

BOOL DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(reason);
    UNREFERENCED_PARAMETER(reserved);
    return TRUE;
}

/* ---- host boundary state (host memory addresses) ---- */
static pal_vbs_shared_t* g_host;      /* the shared struct in HOST memory (E_PalMain param) */
static pal_vbs_ocall_fn  g_ocall;     /* host OCALL dispatcher (host code address) */

/* Perform one OCALL. Args are copied OUT to the host struct, the host dispatcher is called, and the
 * (host-written) args are copied back IN so the enclave can read the return value. The host cannot
 * read enclave memory, so the shared struct is the only channel (phase 0). */
static uint64_t do_ocall(uint64_t no, uint64_t a0, uint64_t a1, uint64_t a2) {
    pal_vbs_ocall_args_t args = { no, a0, a1, a2, 0 };
    EnclaveCopyOutOfEnclave(&g_host->ocall_args, &args, sizeof(args));
    void* ret = NULL;
    CallEnclave((LPENCLAVE_ROUTINE)g_ocall, &g_host->ocall_args, TRUE, &ret);
    EnclaveCopyIntoEnclave(&args, &g_host->ocall_args, sizeof(args));
    return args.retval;
}

/* ---- PAL primitives exposed to the test (pal_vbs_env) ---- */

static void pal_console_write(const char* s, uint64_t len) {
    if (len > PAL_VBS_CONBUF_SIZE)
        len = PAL_VBS_CONBUF_SIZE;
    EnclaveCopyOutOfEnclave(g_host->conbuf, s, (SIZE_T)len);
    do_ocall(PAL_VBS_OCALL_CONSOLE_WRITE, len, 0, 0);
}

/* RW allocator over enclave memory. Phase 0: VirtualAlloc(PAGE_READWRITE) commits up to the enclave
 * size; any execute protection fails with 1655, so this memory is never made executable. */
static void* pal_mem_alloc(uint64_t size) {
    return VirtualAlloc(NULL, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void pal_mem_free(void* p, uint64_t size) {
    UNREFERENCED_PARAMETER(size);
    if (p)
        VirtualFree(p, 0, MEM_RELEASE);
}

static uint64_t pal_time_ns(void) {
    return do_ocall(PAL_VBS_OCALL_GET_TIME_NS, 0, 0, 0);
}

static int64_t pal_random(void* buf, uint64_t len) {
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st) ? 0 : (int64_t)st;
}

/* SEH exception hook: raise a breakpoint and resume past it (phase 0: brk is catchable and the
 * handler can resume at Pc+4). No AddVectoredExceptionHandler exists; SEH only. */
static LONG seh_resume_filter(EXCEPTION_POINTERS* ep, int64_t* code) {
    *code = (int64_t)ep->ExceptionRecord->ExceptionCode;
    ep->ContextRecord->Pc += 4; /* skip the faulting brk */
    return EXCEPTION_CONTINUE_EXECUTION;
}
static int64_t pal_try_brk_resume(void) {
    int64_t code = 0;
    __try {
        pal_vbs_brk0();
    } __except (seh_resume_filter(GetExceptionInformation(), &code)) {
        /* not reached: the filter resumes execution */
    }
    /*
     * We caught an in-enclave synchronous fault and resumed past it (returning here at all proves
     * the resume worked). The fault code is STATUS_BREAKPOINT (0x80000003) when a debugger is
     * attached, but on this machine with no debugger `brk #0` surfaces as STATUS_ILLEGAL_INSTRUCTION
     * (0xC000001D) -- see Documentation/devel/vbs-enclave/FINDINGS.md. Accept either trap.
     */
    if (code == (int64_t)0x80000003 || code == (int64_t)0xC000001D)
        return 0;
    return code ? code : -1;
}

#ifdef PAL_VBS_NO_PACKED
/* An image built without packed objects (scripts/vbs/build-objs.sh without PACK): no loader. */
uint64_t pal_vbs_packed_count(void) { return 0; }
const char* pal_vbs_packed_name(uint64_t i) { UNREFERENCED_PARAMETER(i); return NULL; }
int64_t pal_vbs_packed_load(const char* name, const struct pal_vbs_import* imports,
                            uint64_t nimports, struct pal_vbs_loaded* out, uint64_t* detail) {
    UNREFERENCED_PARAMETER(name); UNREFERENCED_PARAMETER(imports);
    UNREFERENCED_PARAMETER(nimports); UNREFERENCED_PARAMETER(out); UNREFERENCED_PARAMETER(detail);
    return PAL_VBS_PK_NOTFOUND;
}
void* pal_vbs_packed_sym(const struct pal_vbs_loaded* obj, const char* sym) {
    UNREFERENCED_PARAMETER(obj); UNREFERENCED_PARAMETER(sym);
    return NULL;
}
#endif

static const pal_vbs_env_t g_env = {
    .console_write  = pal_console_write,
    .mem_alloc      = pal_mem_alloc,
    .mem_free       = pal_mem_free,
    .time_ns        = pal_time_ns,
    .random         = pal_random,
    .enter_sysv     = pal_vbs_enter_sysv,
    .read_x18       = pal_vbs_read_x18,
    .try_brk_resume = pal_try_brk_resume,
    .packed_count   = pal_vbs_packed_count,
    .packed_name    = pal_vbs_packed_name,
    .packed_load    = pal_vbs_packed_load,
    .packed_sym     = pal_vbs_packed_sym,
};

/*
 * E_PalMain: the single phase-1 ECALL. The host passes &pal_vbs_shared (host memory). We copy it in
 * to learn the OCALL dispatcher, run the PAL-level self-test wrapped in SEH so a fault is reported
 * rather than hanging, and write the result back out. The return value also carries the result.
 */
void* CALLBACK E_PalMain(void* param) {
    pal_vbs_shared_t local;
    g_host = (pal_vbs_shared_t*)param;
    EnclaveCopyIntoEnclave(&local, g_host, sizeof(local));
    g_ocall = (pal_vbs_ocall_fn)local.ocall;

    int64_t result = -1;
    uint64_t run = 0, passed = 0;
    __try {
        result = pal_vbs_selftest(&g_env, &run, &passed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = (int64_t)0x1000 | (int64_t)GetExceptionCode();
    }

    /* write results back to host memory */
    EnclaveCopyOutOfEnclave(&g_host->result, &result, sizeof(result));
    EnclaveCopyOutOfEnclave(&g_host->selftests_run, &run, sizeof(run));
    EnclaveCopyOutOfEnclave(&g_host->selftests_passed, &passed, sizeof(passed));

    /* tell the host we are done (process-exit OCALL with the result as the code) */
    do_ocall(PAL_VBS_OCALL_EXIT, (uint64_t)result, 0, 0);
    return (void*)(intptr_t)result;
}
