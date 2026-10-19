/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Enclave side of the Windows VBS-enclave PAL (phase 1 skeleton).
 *
 * This is a signed ARM64 enclave DLL. It is NOT yet wired to Gramine's common PAL sources or the
 * LibOS (that needs the phase-2 packer and loader changes); phase 1 proves the host<->enclave
 * boundary: the PAL entry (E_PalMain ECALL), the Windows<->System-V ABI thunk with x18 preserved,
 * an SEH exception hook, console/time/exit via OCALLs, an RW memory allocator and random.
 *
 * The enclave configuration and DLL skeleton follow Microsoft's VbsEnclave sample (MIT); see
 * test/vbs/arm64/step0-sample/.
 */

#include <winenclave.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pal_vbs_abi.h"
#include "pal_vbs_enclave.h"

/* the enclave configuration chosen at pack time from the manifest (vbs.max_threads, ...), if any */
#if defined(__has_include)
#if __has_include("pal_vbs_image_config.h")
#include "pal_vbs_image_config.h"
#endif
#endif

/* resume after the trapping instruction of the probes: brk (4 bytes) on AArch64, ud2 (2) on x64 */
#if defined(_M_X64)
#define PAL_VBS_SKIP_TRAP(ctx) ((ctx)->Rip += 2)
#else
#define PAL_VBS_SKIP_TRAP(ctx) ((ctx)->Pc += 4)
#endif

#ifndef PAL_VBS_POLICY
#define PAL_VBS_POLICY IMAGE_ENCLAVE_POLICY_DEBUGGABLE /* DO NOT SHIP DEBUGGABLE ENCLAVES */
#endif
#ifndef PAL_VBS_SIZE
#define PAL_VBS_SIZE 0x10000000 /* 256 MiB */
#endif
#ifndef PAL_VBS_THREADS
#define PAL_VBS_THREADS 4
#endif
#ifndef PAL_VBS_FAMILY_ID
#define PAL_VBS_FAMILY_ID { 0xFE, 0xFE }
#endif
#ifndef PAL_VBS_IMAGE_ID
#define PAL_VBS_IMAGE_ID { 0x01, 0x04 }
#endif
#ifndef PAL_VBS_SVN
#define PAL_VBS_SVN 0
#endif

const IMAGE_ENCLAVE_CONFIG __enclave_config = {
    sizeof(IMAGE_ENCLAVE_CONFIG),
    IMAGE_ENCLAVE_MINIMUM_CONFIG_SIZE,
    PAL_VBS_POLICY,
    0, 0, 0,
    PAL_VBS_FAMILY_ID, /* family id (vbs.family_id; without a manifest: the self-test's) */
    PAL_VBS_IMAGE_ID,  /* image id (vbs.image_id) */
    0,                 /* version */
    PAL_VBS_SVN,       /* SVN (vbs.isv_svn) */
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

/*
 * Threads inside the enclave and their host-side OCALL blocks. Every enclave thread has its own
 * argument block and console buffer in host memory (the first thread: in pal_vbs_shared; others: a
 * pal_vbs_thread_block the host allocated), found through the thread's TEB (x18). Entries are
 * written before the thread makes its first OCALL and cleared when it leaves the enclave.
 */
#define W_MAX_THREADS 256
struct w_thread {
    volatile uint64_t teb;
    pal_vbs_ocall_args_t* host_args;
    uint8_t* host_conbuf;
    uint8_t* host_iobuf;
    void* exit_state;   /* E_ThreadStart's context of the thread (NULL: the first thread) */
};
static struct w_thread g_threads[W_MAX_THREADS];

static uint64_t current_teb(void) {
    return (uint64_t)(uintptr_t)NtCurrentTeb();
}

int64_t pal_vbs_w_register_thread(pal_vbs_ocall_args_t* host_args, uint8_t* host_conbuf,
                                  uint8_t* host_iobuf) {
    uint64_t teb = current_teb();
    for (int i = 0; i < W_MAX_THREADS; i++) {
        if (InterlockedCompareExchange64((volatile LONG64*)&g_threads[i].teb, (LONG64)teb, 0) == 0) {
            g_threads[i].host_args = host_args;
            g_threads[i].host_conbuf = host_conbuf;
            g_threads[i].host_iobuf = host_iobuf;
            g_threads[i].exit_state = NULL;
            return 0;
        }
    }
    return -1;
}

void pal_vbs_w_unregister_thread(void) {
    uint64_t teb = current_teb();
    for (int i = 0; i < W_MAX_THREADS; i++)
        if (g_threads[i].teb == teb) {
            g_threads[i].host_args = NULL;
            g_threads[i].host_conbuf = NULL;
            InterlockedExchange64((volatile LONG64*)&g_threads[i].teb, 0);
            return;
        }
}

void pal_vbs_w_set_exit_state(void* st) {
    uint64_t teb = current_teb();
    for (int i = 0; i < W_MAX_THREADS; i++)
        if (g_threads[i].teb == teb)
            g_threads[i].exit_state = st;
}

uint8_t* pal_vbs_w_get_iobuf(void) {
    uint64_t teb = current_teb();
    for (int i = 0; i < W_MAX_THREADS; i++)
        if (g_threads[i].teb == teb)
            return g_threads[i].host_iobuf;
    return NULL;
}

void* pal_vbs_w_get_exit_state(void) {
    uint64_t teb = current_teb();
    for (int i = 0; i < W_MAX_THREADS; i++)
        if (g_threads[i].teb == teb)
            return g_threads[i].exit_state;
    return NULL;
}

static struct w_thread* this_thread(void) {
    uint64_t teb = current_teb();
    for (int i = 0; i < W_MAX_THREADS; i++)
        if (g_threads[i].teb == teb && g_threads[i].host_args)
            return &g_threads[i];
    return NULL;
}

/* Perform one OCALL. Args are copied OUT to the calling thread's host block, the host dispatcher is
 * called, and the (host-written) args are copied back IN so the enclave can read the return value.
 * The host cannot read enclave memory, so the host blocks are the only channel (phase 0). */
uint64_t pal_vbs_w_ocall(uint64_t no, uint64_t a0, uint64_t a1, uint64_t a2) {
    struct w_thread* t = this_thread();
    if (!t)
        return (uint64_t)-1; /* a thread the enclave does not know: no channel to the host */
    pal_vbs_ocall_args_t args = { no, a0, a1, a2, 0 };
    EnclaveCopyOutOfEnclave(t->host_args, &args, sizeof(args));
    void* ret = NULL;
#if defined(_M_X64)
    /* the FS base (the Linux thread pointer) can come back stale from an OCALL (FINDINGS-x64.md) */
    uint64_t fs;
    __asm__ volatile("rdfsbase %0" : "=r"(fs));
    CallEnclave((LPENCLAVE_ROUTINE)g_ocall, t->host_args, TRUE, &ret);
    __asm__ volatile("wrfsbase %0" : : "r"(fs) : "memory");
#else
    CallEnclave((LPENCLAVE_ROUTINE)g_ocall, t->host_args, TRUE, &ret);
#endif
    EnclaveCopyIntoEnclave(&args, t->host_args, sizeof(args));
    return args.retval;
}

static uint64_t do_ocall(uint64_t no, uint64_t a0, uint64_t a1, uint64_t a2) {
    return pal_vbs_w_ocall(no, a0, a1, a2);
}

/* ---- PAL primitives exposed to the test (pal_vbs_env) ---- */

void pal_vbs_w_console_write(const char* s, uint64_t len) {
    struct w_thread* t = this_thread();
    if (!t)
        return;
    if (len > PAL_VBS_CONBUF_SIZE)
        len = PAL_VBS_CONBUF_SIZE;
    EnclaveCopyOutOfEnclave(t->host_conbuf, s, (SIZE_T)len);
    do_ocall(PAL_VBS_OCALL_CONSOLE_WRITE, len, (uint64_t)(uintptr_t)t->host_conbuf, 0);
}

void pal_vbs_w_log(const char* s) {
    pal_vbs_w_console_write(s, (uint64_t)strlen(s));
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

uint64_t pal_vbs_w_time_ns(void) {
    return do_ocall(PAL_VBS_OCALL_GET_TIME_NS, 0, 0, 0);
}

int64_t pal_vbs_w_random(void* buf, uint64_t len) {
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st) ? 0 : (int64_t)st;
}

/* SEH exception hook: raise a breakpoint and resume past it (phase 0: brk is catchable and the
 * handler can resume at Pc+4). No AddVectoredExceptionHandler exists; SEH only. */
static LONG seh_resume_filter(EXCEPTION_POINTERS* ep, int64_t* code) {
    *code = (int64_t)ep->ExceptionRecord->ExceptionCode;
    PAL_VBS_SKIP_TRAP(ep->ContextRecord); /* skip the faulting brk */
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
const struct pal_vbs_packed_obj* pal_vbs_packed_table(uint64_t* count, uint64_t* blob_base,
                                                       uint64_t* blob_size, uint64_t* files_base,
                                                       uint64_t* files_size) {
    *count = *blob_base = *blob_size = *files_base = *files_size = 0;
    return NULL;
}
#if defined(_M_X64)
int pal_vbs_is_syscall_site(uint64_t rip) { UNREFERENCED_PARAMETER(rip); return 0; }
#endif
#endif

static int64_t pal_spin_foreign_stack(uint64_t iterations) {
    void* stk = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stk)
        return -1;
    pal_vbs_spin_on_stack(iterations, (uint8_t*)stk + 0x10000);
    VirtualFree(stk, 0, MEM_RELEASE);
    return 0;
}

static int64_t pal_brk_foreign_stack(void) {
    void* stk = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stk)
        return -1;
    int64_t code = 0;
    /* as fibers do: tell the exception dispatcher (TEB stack bounds) about the stack in use */
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    void* saved_base = tib->StackBase;
    void* saved_limit = tib->StackLimit;
    tib->StackLimit = stk;
    tib->StackBase = (uint8_t*)stk + 0x10000;
    __try {
        pal_vbs_brk_on_stack((uint8_t*)stk + 0x10000);
    } __except (seh_resume_filter(GetExceptionInformation(), &code)) {
    }
    tib->StackBase = saved_base;
    tib->StackLimit = saved_limit;
    VirtualFree(stk, 0, MEM_RELEASE);
    return (code == (int64_t)0x80000003 || code == (int64_t)0xC000001D) ? 0 : (code ? code : -2);
}

/* language-specific handler of pal_vbs_brk_probe: resume past the brk */
static volatile int64_t g_probe_code;
EXCEPTION_DISPOSITION pal_vbs_probe_handler(PEXCEPTION_RECORD rec, void* frame, PCONTEXT ctx,
                                            void* dispatcher_ctx) {
    UNREFERENCED_PARAMETER(frame);
    UNREFERENCED_PARAMETER(dispatcher_ctx);
    g_probe_code = (int64_t)rec->ExceptionCode;
    PAL_VBS_SKIP_TRAP(ctx);
    return ExceptionContinueExecution;
}

/* brk with SP outside the Windows stack, in a function whose unwind data names a handler;
 * widen = 1: the TEB stack bounds cover the stack in use (as fibers do) */
static int64_t pal_brk_probe(int widen) {
    void* stk = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stk)
        return -1;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    void* saved_base = tib->StackBase;
    void* saved_limit = tib->StackLimit;
    if (widen) {
        tib->StackLimit = stk;
        tib->StackBase = (uint8_t*)stk + 0x10000;
    }
    g_probe_code = 0;
    pal_vbs_brk_probe((uint8_t*)stk + 0x10000);
    tib->StackBase = saved_base;
    tib->StackLimit = saved_limit;
    VirtualFree(stk, 0, MEM_RELEASE);
    int64_t code = g_probe_code;
    return (code == (int64_t)0x80000003 || code == (int64_t)0xC000001D) ? 0 : (code ? code : -2);
}

static const pal_vbs_env_t g_env = {
    .console_write  = pal_vbs_w_console_write,
    .mem_alloc      = pal_mem_alloc,
    .mem_free       = pal_mem_free,
    .time_ns        = pal_vbs_w_time_ns,
    .random         = pal_vbs_w_random,
    .enter_sysv     = pal_vbs_enter_sysv,
    .read_x18       = pal_vbs_read_x18,
    .try_brk_resume = pal_try_brk_resume,
    .packed_count   = pal_vbs_packed_count,
    .packed_name    = pal_vbs_packed_name,
    .packed_load    = pal_vbs_packed_load,
    .packed_sym     = pal_vbs_packed_sym,
    .x18_spin       = pal_vbs_x18_spin,
    .spin_foreign_stack = pal_spin_foreign_stack,
    .brk_foreign_stack  = pal_brk_foreign_stack,
    .brk_probe          = pal_brk_probe,
    .regs_spin          = pal_vbs_regs_spin,
    .get_tpidr          = pal_vbs_get_tpidr,
    .set_tpidr          = pal_vbs_set_tpidr,
};

/*
 * E_PalMain: the single phase-1 ECALL. The host passes &pal_vbs_shared (host memory). We copy it in
 * to learn the OCALL dispatcher, run the PAL-level self-test wrapped in SEH so a fault is reported
 * rather than hanging, and write the result back out. The return value also carries the result.
 */
void* CALLBACK E_PalMain(void* param) {
    /* static, not on the 8 KiB Windows stack of the enclave thread (FINDINGS.md); E_PalMain runs once */
    static pal_vbs_shared_t local;
    g_host = (pal_vbs_shared_t*)param;
    EnclaveCopyIntoEnclave(&local, g_host, sizeof(local));
    g_ocall = (pal_vbs_ocall_fn)local.ocall;
    pal_vbs_w_register_thread(&g_host->ocall_args, g_host->conbuf, g_host->iobuf);

    int64_t result = -1;
    uint64_t run = 0, passed = 0;
    if (local.abi_version != PAL_VBS_ABI_VERSION) {
        result = -2;
    } else if (local.mode == PAL_VBS_MODE_TEBSLOT) {
        /* measurement: does NT_TIB.ArbitraryUserPointer (the packed code's thread pointer) survive
         * preemption, OCALLs and waits? */
        volatile void** slot = (volatile void**)&((NT_TIB*)NtCurrentTeb())->ArbitraryUserPointer;
        void* saved = (void*)*slot;
        uint64_t miss = 0, first = 0, ocalls = 0;
        uint64_t t0 = pal_vbs_w_time_ns();
        for (uint64_t i = 1; i <= 3000000000ull; i++) {
            void* want = (void*)(uintptr_t)(0x7ee4000000000000ull + (i >> 12));
            if ((i & 0xfff) == 0) {
                *slot = want;
                (void)pal_vbs_w_time_ns(); /* an OCALL */
                uint32_t z = 0, zz = 0;
                WaitOnAddress(&z, &zz, sizeof(z), 0); /* a wait, returns at once */
                ocalls++;
            }
            void* v = (void*)*slot;
            if (v != (void*)(uintptr_t)(0x7ee4000000000000ull + (i >> 12)) && (i & 0xfff)) {
                if (!miss)
                    first = (uint64_t)(uintptr_t)v;
                miss++;
                *slot = (void*)(uintptr_t)(0x7ee4000000000000ull + (i >> 12));
            }
        }
        uint64_t t1 = pal_vbs_w_time_ns();
        *slot = saved;
        char buf[200];
        snprintf(buf, sizeof(buf), "[tebslot] %llu ms, %llu OCALLs+waits: ArbitraryUserPointer "
                 "changed %llu times, first value 0x%llx\n", (unsigned long long)((t1 - t0) / 1000000),
                 (unsigned long long)ocalls, (unsigned long long)miss, (unsigned long long)first);
        pal_vbs_w_log(buf);
        result = (int64_t)miss;
        } else if (local.mode == PAL_VBS_MODE_TPIDR) {
        uint64_t first = 0;
        uint64_t t0 = pal_vbs_w_time_ns();
        uint64_t miss = pal_vbs_tpidr_spin(30000000000ull, 0x7ee3000000000001ull, &first);
        uint64_t t1 = pal_vbs_w_time_ns();
        char buf[160];
        snprintf(buf, sizeof(buf), "[tpidr] %llu ms of spinning: TPIDR_EL0 changed %llu times, "
                 "first value seen 0x%llx\n", (unsigned long long)((t1 - t0) / 1000000),
                 (unsigned long long)miss, (unsigned long long)first);
        pal_vbs_w_log(buf);
        result = (int64_t)miss;
        } else if (local.mode == PAL_VBS_MODE_BOOT) {
        /* SEH around the boot as around the self-test: an exception the PAL does not handle is
         * reported instead of being lost (some faults hang instead, blocker 4) */
        __try {
            result = pal_vbs_boot(&local);
        } __except (pal_vbs_report_exception(GetExceptionInformation(), "the first thread")) {
            result = (int64_t)0x1000 | (int64_t)GetExceptionCode();
        }
    } else {
        __try {
            result = pal_vbs_selftest(&g_env, &run, &passed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result = (int64_t)0x1000 | (int64_t)GetExceptionCode();
        }
    }

    /* write results back to host memory */
    EnclaveCopyOutOfEnclave(&g_host->result, &result, sizeof(result));
    EnclaveCopyOutOfEnclave(&g_host->selftests_run, &run, sizeof(run));
    EnclaveCopyOutOfEnclave(&g_host->selftests_passed, &passed, sizeof(passed));

    /* tell the host we are done (process-exit OCALL with the result as the code) */
    do_ocall(PAL_VBS_OCALL_EXIT, (uint64_t)result, 0, 0);
    pal_vbs_w_unregister_thread();
    return (void*)(intptr_t)result;
}
