/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

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

#if defined(_M_X64)
/* where the PAL_TCB of a thread lives in its TEB on x64 (PLAN-x64.md, X2; must match
 * pal/include/arch/x86_64/pal_tcb_gs.h), and the offset in it of the LibOS's syscall entry
 * (GRAMINE_SYSCALL_OFFSET of libos/include/arch/x86_64/gramine_entry_api.h) */
#define PAL_VBS_TEB_TCB_OFFSET    0x800
#define PAL_VBS_TCB_SYSCALL_ENTRY 24
#endif

#ifndef PAL_VBS_POOL_SIZE
#define PAL_VBS_POOL_SIZE 0x8000000ull /* at least 128 MiB of the enclave committed for the PAL */
#endif
#define PAL_VBS_W_RESERVE 0x4000000ull /* 64 MiB of the enclave left to W */

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

/* The PAL's clock without an OCALL per reading. The host's Unix time (an OCALL, about 8 us) is taken
 * at most once a second; in between, the time advances by the generic timer's virtual counter
 * (CNTVCT_EL0, readable in the enclave; CNTFRQ_EL0 gives its rate). A seqlock publishes the
 * (host time, counter) pair; readings never go backwards, also across a resynchronisation. The
 * host's time is untrusted either way (as with the OCALL for every reading). */
static volatile uint64_t g_clk_seq;  /* odd while being updated; 0: never synchronised */
static volatile uint64_t g_clk_ns;
static volatile uint64_t g_clk_cnt;
static volatile uint64_t g_clk_last; /* the latest reading returned */
static uint64_t g_clk_frq;

#if defined(_M_X64)
/* x64: the counter is QueryPerformanceCounter (vertdll; no generic timer) */
static inline uint64_t clk_counter(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
static inline uint64_t clk_frequency(void) {
    LARGE_INTEGER f;
    return QueryPerformanceFrequency(&f) ? (uint64_t)f.QuadPart : 0;
}
#else
static inline uint64_t clk_counter(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
#endif

static uint64_t svc_time_ns(void) {
    uint64_t frq = __atomic_load_n(&g_clk_frq, __ATOMIC_RELAXED);
    if (!frq) {
#if defined(_M_X64)
        frq = clk_frequency();
#else
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
#endif
        if (!frq || frq > 1000000000ull)
            return pal_vbs_w_time_ns(); /* no usable counter: an OCALL per reading */
        __atomic_store_n(&g_clk_frq, frq, __ATOMIC_RELAXED);
    }
    uint64_t t;
    uint64_t seq = __atomic_load_n(&g_clk_seq, __ATOMIC_ACQUIRE);
    uint64_t base_ns = __atomic_load_n(&g_clk_ns, __ATOMIC_RELAXED);
    uint64_t base_cnt = __atomic_load_n(&g_clk_cnt, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t cnt = clk_counter();
    if (seq && !(seq & 1) && __atomic_load_n(&g_clk_seq, __ATOMIC_RELAXED) == seq &&
            cnt >= base_cnt && cnt - base_cnt < frq) {
        /* less than a second of counter ticks: (ticks * 1e9) fits in 64 bits */
        t = base_ns + (cnt - base_cnt) * 1000000000ull / frq;
    } else {
        t = pal_vbs_w_time_ns();
        cnt = clk_counter();
        if (!(seq & 1) && __atomic_compare_exchange_n(&g_clk_seq, &seq, seq + 1, false,
                                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_clk_ns, t, __ATOMIC_RELAXED);
            __atomic_store_n(&g_clk_cnt, cnt, __ATOMIC_RELAXED);
            __atomic_store_n(&g_clk_seq, seq + 2, __ATOMIC_RELEASE);
        }
    }
    uint64_t last = __atomic_load_n(&g_clk_last, __ATOMIC_RELAXED);
    while (t > last && !__atomic_compare_exchange_n(&g_clk_last, &last, t, true, __ATOMIC_RELAXED,
                                                    __ATOMIC_RELAXED))
        ;
    return t > last ? t : last;
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
    if (p == PAGE_READONLY) {
        /* A write to a page that went straight from read-write to read-only hangs the thread
         * instead of raising an access violation (FINDINGS.md, signals test); through no-access
         * first, the same write faults normally. */
        (void)VirtualProtect((void*)(uintptr_t)addr, (SIZE_T)size, PAGE_NOACCESS, &old);
    }
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
#if defined(_M_X64)
    log_kv("  rip ", ep->ContextRecord->Rip);
    log_kv("  rsp ", ep->ContextRecord->Rsp);
    log_kv("  rax ", ep->ContextRecord->Rax);
    log_kv("  rdi ", ep->ContextRecord->Rdi);
#else
    log_kv("  pc  ", ep->ContextRecord->Pc);
    log_kv("  sp  ", ep->ContextRecord->Sp);
    log_kv("  lr  ", ep->ContextRecord->Lr);
    log_kv("  x0  ", ep->ContextRecord->X0);
#endif
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
#if defined(_M_X64)
/*
 * x64 (PLAN-x64.md, X1 and X3). Three things reach this handler besides real faults:
 *  - a lost FS base: preemption resets the FS base to 0 and an OCALL may leave a stale value
 *    (FINDINGS-x64.md); the packed code's thread pointer is kept in NT_TIB.ArbitraryUserPointer, so
 *    a fault with a different FS base is repaired and retried (a wrfsbase in the handler survives
 *    the resume);
 *  - a system call: pack.py rewrote every `syscall` of the packed code into `ud2` (same 2 bytes) and
 *    listed the sites; the thread enters the LibOS's syscall entry with rcx = the next instruction
 *    and r11 = the flags, which is what `syscall` would have left. The entry is the LibOS TCB's
 *    syscall-entry field of the thread's PAL_TCB, which lives in the TEB (X2);
 *  - everything else goes to the PAL as on AArch64.
 */
static inline uint64_t rd_fsbase(void) {
    uint64_t v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
}
static inline void wr_fsbase(uint64_t v) {
    __asm__ volatile("wrfsbase %0" : : "r"(v) : "memory");
}

EXCEPTION_DISPOSITION pal_vbs_packed_exception_handler(PEXCEPTION_RECORD rec, void* frame,
                                                       PCONTEXT ctx, void* dispatcher_ctx) {
    UNREFERENCED_PARAMETER(frame);
    UNREFERENCED_PARAMETER(dispatcher_ctx);
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        return ExceptionContinueSearch;

    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uint64_t fs = (uint64_t)(uintptr_t)tib->ArbitraryUserPointer;
    if (fs && rd_fsbase() != fs) {
        wr_fsbase(fs);
        return ExceptionContinueExecution;
    }
    if (!g_pal_exception_entry)
        return ExceptionContinueSearch;

    if (rec->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION && pal_vbs_is_syscall_site(ctx->Rip)) {
        uint64_t entry = *(const uint64_t*)((const uint8_t*)tib + PAL_VBS_TEB_TCB_OFFSET +
                                            PAL_VBS_TCB_SYSCALL_ENTRY);
        if (entry) {
            ctx->Rcx = ctx->Rip + 2;
            ctx->R11 = ctx->EFlags;
            ctx->Rip = entry;
            return ExceptionContinueExecution;
        }
    }

    struct pal_vbs_fault f;
    memset(&f, 0, sizeof(f));
    f.code = rec->ExceptionCode;
    f.addr = ctx->Rip;
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
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_STACK_CHECK:
            f.kind = PAL_VBS_FAULT_ARITHMETIC;
            break;
        default:
            return ExceptionContinueSearch;
    }
    memcpy(f.regs, &ctx->Rax, sizeof(f.regs)); /* Rax..R15 are contiguous in CONTEXT */
    f.rip = ctx->Rip;
    f.rflags = ctx->EFlags;
    f.fsbase = fs ? fs : rd_fsbase();
    memcpy(f.fxsave, &ctx->FltSave, sizeof(f.fxsave));

    int64_t ret = (int64_t)pal_vbs_enter_sysv(g_pal_exception_entry, (uint64_t)(uintptr_t)&f,
                                              0, 0, 0);
    if (ret < 0)
        return ExceptionContinueSearch; /* the PAL did not handle it: the process ends */

    memcpy(&ctx->Rax, f.regs, sizeof(f.regs));
    ctx->Rip = f.rip;
    /* the PAL may change the arithmetic flags, DF and TF; never IOPL, IF or other system flags */
    ctx->EFlags = (ctx->EFlags & ~0x4dd5u) | ((DWORD)f.rflags & 0x4dd5u);
    memcpy(&ctx->FltSave, f.fxsave, sizeof(f.fxsave));
    ctx->MxCsr = ctx->FltSave.MxCsr;
    if (f.fsbase != fs) {
        tib->ArbitraryUserPointer = (void*)(uintptr_t)f.fsbase;
        wr_fsbase(f.fsbase);
    }
    return ExceptionContinueExecution;
}
#else
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
        if (i != 18) /* x18 stays this thread's TEB, whatever context the PAL resumes */
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
#endif

/* ---- host requests (files, sockets) ---- */

/* Copies the request into the calling thread's I/O buffer in host memory, lets the host serve it,
 * and copies the result and at most out_cap output bytes back (the host's lengths are clamped). */
static int64_t svc_host_call(struct pal_vbs_hostreq* req, void* out, uint64_t out_cap) {
    if (req->in_len > PAL_VBS_HOST_DATA_MAX)
        return -22; /* -EINVAL */
    uint8_t* hb = pal_vbs_w_get_iobuf();
    if (!hb)
        return -5; /* -EIO: no channel to the host for this thread */
    EnclaveCopyOutOfEnclave(hb, req, (SIZE_T)(sizeof(*req) + req->in_len));
    pal_vbs_w_ocall(PAL_VBS_OCALL_HOST, (uint64_t)(uintptr_t)hb, 0, 0);
    struct pal_vbs_hostreq h;
    EnclaveCopyIntoEnclave(&h, hb, sizeof(h));
    uint64_t n = h.out_len;
    if (n > out_cap)
        n = out_cap;
    if (n > PAL_VBS_HOST_DATA_MAX)
        n = PAL_VBS_HOST_DATA_MAX;
    if (n)
        EnclaveCopyIntoEnclave(out, hb + sizeof(h), (SIZE_T)n);
    req->out_len = n;
    req->result = h.result;
    return h.result;
}

/* The same with scatter/gather: the input goes straight from the caller's buffers into the host
 * buffer and the output straight back, without a staging copy in the enclave. */
static int64_t svc_host_call_v(struct pal_vbs_hostcall_v* c) {
    struct pal_vbs_hostreq* req = c->req;
    uint8_t* hb = pal_vbs_w_get_iobuf();
    if (!hb)
        return -5; /* -EIO */
    if (c->in_pre_len > PAL_VBS_HOST_DATA_MAX || c->out_pre_cap > PAL_VBS_HOST_DATA_MAX)
        return -22; /* -EINVAL */
    uint64_t pos = 0;
    uint8_t* data = hb + sizeof(*req);
    if (c->in_pre_len) {
        EnclaveCopyOutOfEnclave(data, c->in_pre, (SIZE_T)c->in_pre_len);
        pos = c->in_pre_len;
    }
    for (uint64_t i = 0; i < c->in_niov && pos < PAL_VBS_HOST_DATA_MAX; i++) {
        uint64_t n = c->in_iov[i].len;
        if (n > PAL_VBS_HOST_DATA_MAX - pos)
            n = PAL_VBS_HOST_DATA_MAX - pos;
        if (n)
            EnclaveCopyOutOfEnclave(data + pos, (const void*)(uintptr_t)c->in_iov[i].base,
                                    (SIZE_T)n);
        pos += n;
    }
    req->in_len = pos;
    EnclaveCopyOutOfEnclave(hb, req, sizeof(*req));
    pal_vbs_w_ocall(PAL_VBS_OCALL_HOST, (uint64_t)(uintptr_t)hb, 0, 0);
    struct pal_vbs_hostreq h;
    EnclaveCopyIntoEnclave(&h, hb, sizeof(h));
    uint64_t n = h.out_len; /* the host's length: clamped to the buffer */
    if (n > PAL_VBS_HOST_DATA_MAX)
        n = PAL_VBS_HOST_DATA_MAX;
    uint64_t done = 0;
    uint64_t k = n < c->out_pre_cap ? n : c->out_pre_cap;
    if (k) {
        EnclaveCopyIntoEnclave(c->out_pre, data, (SIZE_T)k);
        done = k;
    }
    for (uint64_t i = 0; i < c->out_niov && done < n; i++) {
        k = c->out_iov[i].len;
        if (k > n - done)
            k = n - done;
        if (k)
            EnclaveCopyIntoEnclave((void*)(uintptr_t)c->out_iov[i].base, data + done, (SIZE_T)k);
        done += k;
    }
    c->out_len = done;
    req->out_len = done;
    req->result = h.result;
    return h.result;
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

/* ---- attestation ---- */

static int64_t svc_attest(const uint8_t* user_data, void* out, uint64_t out_cap) {
    UINT8 data[ENCLAVE_REPORT_DATA_LENGTH];
    memcpy(data, user_data, sizeof(data));
    UINT32 size = 0;
    HRESULT hr = EnclaveGetAttestationReport(data, NULL, 0, &size);
    if (FAILED(hr))
        return -(int64_t)(hr & 0x7fffffff);
    if (!out || out_cap < size)
        return size;
    hr = EnclaveGetAttestationReport(data, out, (UINT32)out_cap, &size);
    if (FAILED(hr))
        return -(int64_t)(hr & 0x7fffffff);
    return size;
}

/*
 * E_VerifyReport: verifies a report package with the platform (the signature chain to the Secure
 * Kernel) and returns the verifying enclave's identity. Needs no PAL: the host calls it in a fresh
 * enclave (`pal_vbs_host.exe <image> 1 verify ...`).
 */
void* CALLBACK E_VerifyReport(void* param) {
    struct pal_vbs_verify v;
    EnclaveCopyIntoEnclave(&v, param, sizeof(v));
    static UINT8 report[PAL_VBS_REPORT_MAX];
    if (!v.report_size || v.report_size > sizeof(report))
        return (void*)(intptr_t)-1;
    if (FAILED(EnclaveCopyIntoEnclave(report, (const void*)(uintptr_t)v.report,
                                      (SIZE_T)v.report_size)))
        return (void*)(intptr_t)-2;
    v.hresult = EnclaveVerifyAttestationReport(ENCLAVE_TYPE_VBS, report, (UINT32)v.report_size);
    ENCLAVE_INFORMATION info;
    memset(&info, 0, sizeof(info));
    v.info_hresult = EnclaveGetEnclaveInformation(sizeof(info), &info);
    _Static_assert(sizeof(info.Identity) <= sizeof(v.own_identity), "identity size");
    memset(v.own_identity, 0, sizeof(v.own_identity));
    memcpy(v.own_identity, &info.Identity, sizeof(info.Identity));
    EnclaveCopyOutOfEnclave(param, &v, sizeof(v));
    return 0;
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
    if (pal_vbs_w_register_thread(&host->ocall_args, host->conbuf, host->iobuf) < 0)
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

    /* the pool: the enclave minus the image and a reserve for W (thread stacks, CRT heap) */
    uint64_t pool_size = PAL_VBS_POOL_SIZE;
    if (g_svc.enclave_size > g_svc.image_size + PAL_VBS_W_RESERVE + PAL_VBS_POOL_SIZE)
        pool_size = (g_svc.enclave_size - g_svc.image_size - PAL_VBS_W_RESERVE) & ~0xffffull;
    void* pool = VirtualAlloc(NULL, (SIZE_T)pool_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pool) {
        log_kv("pool VirtualAlloc failed, error", GetLastError());
        return -4;
    }
    g_svc.pool_base = (uint64_t)(uintptr_t)pool;
    g_svc.pool_size = pool_size;

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
    g_svc.host_call     = (void*)svc_host_call;
    g_svc.host_call_v   = (void*)svc_host_call_v;
    g_svc.attest        = (void*)svc_attest;
    /* host arguments: already copied into the enclave with the shared block (E_PalMain); bounded
     * and NUL-terminated here */
    static char args[PAL_VBS_ARGS_SIZE + 1];
    uint64_t alen = shared->args_len > PAL_VBS_ARGS_SIZE ? PAL_VBS_ARGS_SIZE : shared->args_len;
    memcpy(args, shared->args, (size_t)alen);
    args[alen] = '\0';
    g_svc.args = args;
    g_svc.args_len = alen;
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
