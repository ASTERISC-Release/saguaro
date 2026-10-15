/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Boot path of the Windows VBS-enclave PAL (phase 2): the Windows enclave runtime (W) prepares the
 * packed vbs PAL proper ("libpal", an AArch64 Linux ELF packed into this image at build time) with
 * the packed-object loader and enters it with the table of W services (struct pal_vbs_services in
 * pal_vbs_abi.h). The PAL then loads the packed LibOS, which runs the packed application.
 *
 * W keeps every decision about memory protection: the `protect` service refuses any execute
 * permission and any change to the pages of this image outside the packed-images section, so a
 * fault in the Linux code cannot make W's own code or data unusable.
 */

#include <winenclave.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pal_vbs_abi.h"
#include "pal_vbs_enclave.h"
#include "pal_vbs_packed.h"

#ifndef PAL_VBS_POOL_SIZE
#define PAL_VBS_POOL_SIZE 0x8000000ull /* 128 MiB of the enclave committed for the PAL */
#endif

extern IMAGE_DOS_HEADER __ImageBase;

static struct pal_vbs_services g_svc;
static struct pal_vbs_ctx g_boot_ctx;
static int64_t g_exit_code;

static void hex(char* out, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = digits[(v >> (60 - 4 * i)) & 0xf];
    out[18] = 0;
}

static void log_kv(const char* key, uint64_t v) {
    char buf[20];
    hex(buf, v);
    pal_vbs_w_log("[vbs-w] ");
    pal_vbs_w_log(key);
    pal_vbs_w_log(" ");
    pal_vbs_w_log(buf);
    pal_vbs_w_log("\n");
}

/* ---- services for the vbs PAL proper (Windows ARM64 functions called from Linux code) ---- */

static void svc_console_write(const char* buf, uint64_t len) {
    while (len) {
        uint64_t n = len > PAL_VBS_CONBUF_SIZE ? PAL_VBS_CONBUF_SIZE : len;
        pal_vbs_w_console_write(buf, n);
        buf += n;
        len -= n;
    }
}

static uint64_t svc_time_ns(void) {
    return pal_vbs_w_time_ns();
}

static int64_t svc_random(void* buf, uint64_t len) {
    return pal_vbs_w_random(buf, len);
}

static int64_t svc_protect(uint64_t addr, uint64_t size, uint64_t prot) {
    if (prot & PAL_VBS_PROT_EXEC)
        return -(int64_t)ERROR_ACCESS_DENIED; /* the platform forbids it; never ask */
    if (!size || addr + size < addr)
        return -(int64_t)ERROR_INVALID_PARAMETER;
    uint64_t end = addr + size;
    uint64_t img_lo = g_svc.image_base, img_hi = g_svc.image_base + g_svc.image_size;
    uint64_t pk_lo = g_svc.pack_base, pk_hi = g_svc.pack_base + g_svc.pack_size;
    if (addr < img_hi && img_lo < end && !(pk_lo <= addr && end <= pk_hi))
        return -(int64_t)ERROR_ACCESS_DENIED; /* this image's own code and data stay as signed */
    DWORD p = (prot & PAL_VBS_PROT_WRITE) ? PAGE_READWRITE
              : (prot & PAL_VBS_PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
    DWORD old = 0;
    if (!VirtualProtect((void*)(uintptr_t)addr, (SIZE_T)size, p, &old))
        return -(int64_t)GetLastError();
    return 0;
}

static uint64_t g_first_teb; /* TEB of the thread that entered E_PalMain */

static void svc_exit(int64_t code) {
    g_exit_code = code;
    if ((uint64_t)(uintptr_t)NtCurrentTeb() == g_first_teb)
        pal_vbs_ctx_resume(&g_boot_ctx, 1); /* E_PalMain returns the exit code */
    /* another thread ends the process: the host exits at once (the first thread may be blocked
     * anywhere in the enclave) */
    pal_vbs_w_ocall(PAL_VBS_OCALL_EXIT_PROCESS, (uint64_t)code, 0, 0);
    for (;;) {
        uint32_t never = 0, zero = 0;
        WaitOnAddress(&never, &zero, sizeof(never), INFINITE);
    }
}

/* Exception filter that reports where an exception the PAL did not handle happened (PC, SP, LR,
 * the faulting address for an access violation). Runs on the faulting thread. */
LONG pal_vbs_report_exception(EXCEPTION_POINTERS* ep, const char* where) {
    pal_vbs_w_log("[vbs-w] unhandled exception in ");
    pal_vbs_w_log(where);
    pal_vbs_w_log("\n");
    log_kv("  code", ep->ExceptionRecord->ExceptionCode);
    log_kv("  pc  ", ep->ContextRecord->Pc);
    log_kv("  sp  ", ep->ContextRecord->Sp);
    log_kv("  lr  ", ep->ContextRecord->Lr);
    log_kv("  x0  ", ep->ContextRecord->X0);
    if (ep->ExceptionRecord->NumberParameters >= 2)
        log_kv("  addr", ep->ExceptionRecord->ExceptionInformation[1]);
    log_kv("  pack", g_svc.pack_base);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- exceptions in packed code ---- */

/*
 * The TEB's stack bounds are what the exception dispatcher checks a frame against; the LibOS's and
 * the application's stacks lie in the pool, so while a thread runs packed code its bounds cover the
 * whole enclave (FINDINGS.md, step 10). Restored when the thread returns to the host.
 */
struct teb_bounds {
    void* base;
    void* limit;
    void* user_pointer;
};

/* Also the thread pointer of the packed code: pack.py rewrote its mrs/msr TPIDR_EL0 into loads and
 * stores of NT_TIB.ArbitraryUserPointer ([x18, #0x28]), because the kernel does not keep TPIDR_EL0
 * reliably for enclave threads (FINDINGS.md). It starts at 0 for every thread entering the PAL. */
static_assert(offsetof(NT_TIB, ArbitraryUserPointer) == 0x28, "pack.py rewrites to [x18, #0x28]");

static void widen_stack_bounds(struct teb_bounds* saved) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    saved->base = tib->StackBase;
    saved->limit = tib->StackLimit;
    saved->user_pointer = tib->ArbitraryUserPointer;
    tib->StackLimit = (void*)(uintptr_t)g_svc.enclave_base;
    tib->StackBase = (void*)(uintptr_t)(g_svc.enclave_base + g_svc.enclave_size);
    tib->ArbitraryUserPointer = NULL;
}

static void restore_stack_bounds(const struct teb_bounds* saved) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    tib->StackBase = saved->base;
    tib->StackLimit = saved->limit;
    tib->ArbitraryUserPointer = saved->user_pointer;
}

static void* g_pal_exception_entry; /* pal_vbs_exception of the packed PAL, NULL before boot */

/*
 * Language-specific exception handler of all packed code (named by the static unwind data that
 * pack.py emits for it). It runs on the faulting thread, about 3 KiB below the faulting SP, and
 * hands the fault to the PAL, which may change the registers (deliver a signal to the
 * application, restore a saved context); the thread then continues with them. Nothing is unwound
 * through the Linux frames, which have no unwind data.
 */
EXCEPTION_DISPOSITION pal_vbs_packed_exception_handler(PEXCEPTION_RECORD rec, void* frame,
                                                       PCONTEXT ctx, void* dispatcher_ctx) {
    UNREFERENCED_PARAMETER(frame);
    UNREFERENCED_PARAMETER(dispatcher_ctx);
    if (!g_pal_exception_entry || (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)))
        return ExceptionContinueSearch;

    struct pal_vbs_fault f;
    memset(&f, 0, sizeof(f));
    f.code = rec->ExceptionCode;
    f.addr = ctx->Pc;
    switch (rec->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            f.kind = PAL_VBS_FAULT_MEMORY;
            if (rec->NumberParameters >= 2) {
                f.access = rec->ExceptionInformation[0];
                f.addr = rec->ExceptionInformation[1];
            }
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_BREAKPOINT:
            f.kind = PAL_VBS_FAULT_ILLEGAL;
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_INEXACT_RESULT:
            f.kind = PAL_VBS_FAULT_ARITHMETIC;
            break;
        default:
            return ExceptionContinueSearch;
    }
    for (int i = 0; i < 29; i++)
        f.regs[i] = ctx->X[i];
    f.regs[29] = ctx->Fp;
    f.regs[30] = ctx->Lr;
    f.sp = ctx->Sp;
    f.pc = ctx->Pc;
    f.pstate = ctx->Cpsr;
    f.fpcr = ctx->Fpcr;
    f.fpsr = ctx->Fpsr;
    memcpy(f.vregs, ctx->V, sizeof(f.vregs));

    int64_t ret = (int64_t)pal_vbs_enter_sysv(g_pal_exception_entry, (uint64_t)(uintptr_t)&f,
                                              0, 0, 0);
    if (ret < 0)
        return ExceptionContinueSearch; /* the PAL did not handle it: the process ends */

    for (int i = 0; i < 29; i++)
        ctx->X[i] = f.regs[i];
    ctx->Fp = f.regs[29];
    ctx->Lr = f.regs[30];
    ctx->Sp = f.sp;
    ctx->Pc = f.pc;
    ctx->Cpsr = (DWORD)f.pstate;
    ctx->Fpcr = f.fpcr;
    ctx->Fpsr = f.fpsr;
    memcpy(ctx->V, f.vregs, sizeof(f.vregs));
    return ExceptionContinueExecution;
}

/* ---- threads ---- */

/*
 * A thread the PAL asked for: entry(arg, ctx) runs on a new host thread that enters through
 * E_ThreadStart. The host only sees an opaque token; the request stays here until a thread
 * claims it, and an unknown or reused token is refused.
 */
#define W_MAX_PENDING 64
struct pending_thread {
    volatile LONG64 token; /* 0 = free */
    void* entry;
    void* arg;
};
static struct pending_thread g_pending[W_MAX_PENDING];
static volatile LONG64 g_next_token = 1;

static bool in_pack_code(uint64_t addr) {
    return addr >= g_svc.pack_base && addr < g_svc.pack_base + g_svc.pack_size;
}

static int64_t svc_thread_create(void* entry, void* arg) {
    if (!in_pack_code((uint64_t)(uintptr_t)entry))
        return -(int64_t)ERROR_INVALID_PARAMETER; /* code outside the packed objects */
    LONG64 token = InterlockedIncrement64(&g_next_token);
    for (int i = 0; i < W_MAX_PENDING; i++) {
        if (InterlockedCompareExchange64(&g_pending[i].token, -1, 0) == 0) {
            g_pending[i].entry = entry;
            g_pending[i].arg = arg;
            InterlockedExchange64(&g_pending[i].token, token);
            uint64_t ret = pal_vbs_w_ocall(PAL_VBS_OCALL_THREAD_CREATE, (uint64_t)token, 0, 0);
            if (ret != 0) {
                InterlockedExchange64(&g_pending[i].token, 0);
                return -(int64_t)ERROR_NOT_ENOUGH_MEMORY;
            }
            return 0;
        }
    }
    return -(int64_t)ERROR_TOO_MANY_THREADS;
}

struct thread_exit_state {
    struct pal_vbs_ctx ctx;   /* E_ThreadStart's context, resumed by svc_thread_exit */
    int32_t* clear_tid;
};

static void svc_thread_exit(void* unused_ctx, int32_t* clear_tid) {
    /* the thread's own context, found by its TEB (the PAL's TCB lookup may already be gone) */
    UNREFERENCED_PARAMETER(unused_ctx);
    void* ctx = pal_vbs_w_get_exit_state();
    if (!ctx) {
        /* the first thread ends while others continue: it stays parked in the enclave (the
         * process ends when a thread calls exit) */
        for (;;) {
            uint32_t never = 0, zero = 0;
            WaitOnAddress(&never, &zero, sizeof(never), INFINITE);
        }
    }
    struct thread_exit_state* st = ctx;
    st->clear_tid = clear_tid;
    pal_vbs_ctx_resume(&st->ctx, 1);
}

static int64_t svc_wait(uint32_t* addr, uint32_t expected, uint64_t timeout_ns) {
    DWORD ms = INFINITE;
    if (timeout_ns != UINT64_MAX) {
        uint64_t m = (timeout_ns + 999999) / 1000000;
        ms = m >= INFINITE ? INFINITE - 1 : (DWORD)m;
    }
    if (WaitOnAddress(addr, &expected, sizeof(expected), ms))
        return 0;
    return GetLastError() == ERROR_TIMEOUT ? -1 : 0;
}

static void svc_wake(uint32_t* addr, uint64_t all) {
    if (all)
        WakeByAddressAll(addr);
    else
        WakeByAddressSingle(addr);
}

/*
 * E_ThreadStart: the ECALL a host thread makes for PAL_VBS_OCALL_THREAD_CREATE. The parameter is
 * the thread's block in host memory. Returns when the thread exits.
 */
void* CALLBACK E_ThreadStart(void* param) {
    pal_vbs_thread_block_t* host = param;
    uint64_t token = 0;
    EnclaveCopyIntoEnclave(&token, &host->token, sizeof(token));
    if (!token || (int64_t)token < 0)
        return (void*)(intptr_t)-1;
    void* entry = NULL;
    void* arg = NULL;
    for (int i = 0; i < W_MAX_PENDING; i++) {
        if (InterlockedCompareExchange64(&g_pending[i].token, -1, (LONG64)token) == (LONG64)token) {
            entry = g_pending[i].entry;
            arg = g_pending[i].arg;
            InterlockedExchange64(&g_pending[i].token, 0);
            break;
        }
    }
    if (!entry)
        return (void*)(intptr_t)-2; /* not a request of this enclave */
    if (pal_vbs_w_register_thread(&host->ocall_args, host->conbuf) < 0)
        return (void*)(intptr_t)-3;

    struct thread_exit_state st;
    st.clear_tid = NULL;
    struct teb_bounds bounds;
    widen_stack_bounds(&bounds);
    pal_vbs_w_set_exit_state(&st);
    if (pal_vbs_ctx_save(&st.ctx) == 0) {
        __try {
            pal_vbs_enter_sysv(entry, (uint64_t)(uintptr_t)arg, (uint64_t)(uintptr_t)&st, 0, 0);
            pal_vbs_w_log("[vbs-w] a thread entry returned\n");
        } __except (pal_vbs_report_exception(GetExceptionInformation(), "an enclave thread")) {
        }
    } else if (st.clear_tid) {
        /* back on this thread's own stack: the PAL thread no longer uses its stack */
        InterlockedExchange((volatile LONG*)st.clear_tid, 0);
        WakeByAddressAll(st.clear_tid);
    }
    restore_stack_bounds(&bounds);
    pal_vbs_w_unregister_thread();
    return NULL;
}

int64_t pal_vbs_boot(const struct pal_vbs_shared* shared) {
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)((const uint8_t*)&__ImageBase + __ImageBase.e_lfanew);

    memset(&g_svc, 0, sizeof(g_svc));
    g_svc.version      = PAL_VBS_SERVICES_VERSION;
    g_svc.enclave_base = shared->enclave_base;
    g_svc.enclave_size = shared->enclave_size;
    g_svc.image_base   = (uint64_t)(uintptr_t)&__ImageBase;
    g_svc.image_size   = nt->OptionalHeader.SizeOfImage;
    g_svc.thread_count = shared->thread_count;
    g_svc.objs = pal_vbs_packed_table(&g_svc.nobjs, &g_svc.pack_base, &g_svc.pack_size,
                                      &g_svc.files_base, &g_svc.files_size);

    /* The enclave range comes from the (untrusted) host; it must at least contain this image. */
    if (g_svc.image_base < g_svc.enclave_base ||
        g_svc.image_base + g_svc.image_size > g_svc.enclave_base + g_svc.enclave_size) {
        pal_vbs_w_log("[vbs-w] the host's enclave range does not contain the image\n");
        return -3;
    }

    void* pool = VirtualAlloc(NULL, (SIZE_T)PAL_VBS_POOL_SIZE, MEM_COMMIT | MEM_RESERVE,
                              PAGE_READWRITE);
    if (!pool) {
        log_kv("pool VirtualAlloc failed, error", GetLastError());
        return -4;
    }
    g_svc.pool_base = (uint64_t)(uintptr_t)pool;
    g_svc.pool_size = PAL_VBS_POOL_SIZE;

    log_kv("enclave", g_svc.enclave_base);
    log_kv("image  ", g_svc.image_base);
    log_kv("pack   ", g_svc.pack_base);
    log_kv("pool   ", g_svc.pool_base);
    {
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        log_kv("stack  ", (uint64_t)(uintptr_t)tib->StackLimit);
        log_kv("stack-top", (uint64_t)(uintptr_t)tib->StackBase);
    }

    struct pal_vbs_loaded pal;
    uint64_t detail = 0;
    int64_t ret = pal_vbs_packed_load("libpal", NULL, 0, &pal, &detail);
    if (ret < 0) {
        log_kv("loading libpal failed, error", (uint64_t)ret);
        log_kv("detail", detail);
        return -5;
    }
    if (!pal.entry) {
        pal_vbs_w_log("[vbs-w] libpal has no entry\n");
        return -6;
    }
    log_kv("libpal ", (uint64_t)(uintptr_t)pal.base);
    log_kv("relocs ", pal.nrela);

    g_svc.self_base     = (uint64_t)(uintptr_t)pal.base;
    g_svc.teb           = pal_vbs_read_x18();
    g_svc.console_write = (void*)svc_console_write;
    g_svc.time_ns       = (void*)svc_time_ns;
    g_svc.random        = (void*)svc_random;
    g_svc.protect       = (void*)svc_protect;
    g_svc.exit          = (void*)svc_exit;
    g_svc.thread_create = (void*)svc_thread_create;
    g_svc.thread_exit   = (void*)svc_thread_exit;
    g_svc.wait          = (void*)svc_wait;
    g_svc.wake          = (void*)svc_wake;
    g_first_teb         = (uint64_t)(uintptr_t)NtCurrentTeb();

    g_pal_exception_entry = pal_vbs_packed_sym(&pal, "pal_vbs_exception");
    if (!g_pal_exception_entry)
        pal_vbs_w_log("[vbs-w] libpal has no pal_vbs_exception: faults end the process\n");

    struct teb_bounds bounds;
    widen_stack_bounds(&bounds);
    if (pal_vbs_ctx_save(&g_boot_ctx) == 0) {
        /* does not return: the PAL ends through svc_exit, which resumes the saved context */
        pal_vbs_enter_sysv(pal.entry, (uint64_t)(uintptr_t)&g_svc, 0, 0, 0);
        restore_stack_bounds(&bounds);
        pal_vbs_w_log("[vbs-w] the PAL entry returned\n");
        return -7;
    }
    restore_stack_bounds(&bounds);
    log_kv("exit code", (uint64_t)g_exit_code);
    return g_exit_code;
}
