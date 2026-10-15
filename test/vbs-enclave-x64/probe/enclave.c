//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
//
// Gramine phase-0 spike: the probe enclave. The enclave configuration and DLL skeleton come from
// Microsoft's VbsEnclave sample (MIT); the probes are new. Every export takes a PROBE_IO pointer in
// host memory and writes its results there.
//
// x64 version (test/vbs-enclave-x64/): the AArch64 probe of test/vbs-enclave/probe/ with the
// architecture-specific probes replaced by their x86_64 counterparts (probe.asm, ml64).
// The x64 changes: Copyright (C) 2026 The ASTERISC Research Lab

#include <winenclave.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>
#include "probe.h"

#ifndef PROBE_POLICY
#ifdef PROBE_STRICT
#define PROBE_POLICY (IMAGE_ENCLAVE_POLICY_DEBUGGABLE | IMAGE_ENCLAVE_POLICY_STRICT_MEMORY)
#else
#define PROBE_POLICY IMAGE_ENCLAVE_POLICY_DEBUGGABLE
#endif
#endif
#ifndef PROBE_SIZE
#define PROBE_SIZE 0x10000000
#endif
#ifndef PROBE_THREADS
#define PROBE_THREADS 16
#endif

const IMAGE_ENCLAVE_CONFIG __enclave_config = {
    sizeof(IMAGE_ENCLAVE_CONFIG),
    IMAGE_ENCLAVE_MINIMUM_CONFIG_SIZE,
    PROBE_POLICY,      // DO NOT SHIP DEBUGGABLE ENCLAVES TO PRODUCTION
    0, 0, 0,
    { 0xFE, 0xFE },    // family id
    { 0x01, 0x01 },    // image id
    0,                 // version
    0,                 // SVN
    PROBE_SIZE,        // size: 256 MiB by default
    PROBE_THREADS,     // number of threads
    IMAGE_ENCLAVE_FLAG_PRIMARY_IMAGE
};

// ml64 helpers (probe.asm)
long long probe_syscall(long long nr, long long a0, long long a1, long long a2);
long long probe_int80(long long nr);
long long probe_int3(void);
long long probe_ud2(void);
long long probe_div0(void);
long long probe_hlt(void);
void probe_cpuid(unsigned leaf, unsigned subleaf, unsigned out[4]);
unsigned long long probe_xgetbv(unsigned idx);
unsigned long long probe_rdfsbase(void);
void probe_wrfsbase(unsigned long long v);
unsigned long long probe_rdgsbase(void);
unsigned long long probe_read_rsp(void);
long long probe_jump(void* fn, long long a0, long long a1);
unsigned long long probe_rdtscp(unsigned* aux);
unsigned long long probe_rdrand(int* ok);
unsigned long long probe_rdseed(int* ok);
long long probe_redzone_ud2(void);
long long probe_redzone_spin(unsigned long long iters);
long long probe_fs_spin(unsigned long long value, unsigned long long iters, unsigned long long out[3]);
long long probe_regs_spin(unsigned long long iters);
long long probe_callout_regs(void* host_fn);
long long probe_fault_offstack(void* new_rsp);
long long probe_fault_offstack_nohandler(void* new_rsp);
extern unsigned long long g_after_syscall_rcx, g_after_syscall_r11;
extern unsigned char gx_code_region[65536];

extern IMAGE_DOS_HEADER __ImageBase;

static char g_log[PROBE_TEXT];
static int g_len;
static PROBE_IO* g_live;   // host buffer that receives the log as it is written (for the host's watchdog)

static void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_log + g_len, sizeof(g_log) - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len += n;
    if (g_len > (int)sizeof(g_log) - 1) g_len = sizeof(g_log) - 1;
    if (g_live) { memcpy(g_live->text, g_log, g_len + 1); g_live->len = g_len; }
}

static void begin_io(PROBE_IO* io) { g_len = 0; g_log[0] = 0; g_live = io; }
#define begin() begin_io(io)

static void finish(PROBE_IO* io)
{
    // copy the log out to host memory with the documented helper
    HRESULT hr = EnclaveCopyOutOfEnclave(io->text, g_log, g_len + 1);
    if (FAILED(hr)) {
        memcpy(io->text, g_log, g_len + 1);
    }
    io->len = g_len;
}

BOOL DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD dwReason, _In_ LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(lpvReserved);
    UNREFERENCED_PARAMETER(dwReason);
    return TRUE;
}

// ---------------------------------------------------------------- (b) calls

void* CALLBACK E_Nop(void* p) { return p; }

typedef void* (WINAPI* HOST_ROUTINE)(void*);

// args[0] = host routine, args[1] = iterations. Calls the host from inside the enclave.
void* CALLBACK E_CallOut(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    begin();
    LPENCLAVE_ROUTINE fn = (LPENCLAVE_ROUTINE)io->args[0];
    unsigned long long n = io->args[1];
    void* ret = NULL;
    BOOL ok = CallEnclave(fn, (void*)0x5151, TRUE, &ret);
    logf("callout: CallEnclave(host fn, 0x5151) ok=%d GetLastError=%lu ret=%p\n", ok, ok ? 0 : GetLastError(), ret);
    if (ok && n) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        unsigned long long bad = 0;
        for (unsigned long long i = 0; i < n; i++) {
            if (!CallEnclave(fn, (void*)i, TRUE, &ret) || ret != (void*)(i + 1)) bad++;
        }
        QueryPerformanceCounter(&t1);
        double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)f.QuadPart / (double)n;
        logf("callout: %llu calls, %llu wrong, %.0f ns per enclave->host->enclave call (QPC freq %lld)\n", n, bad, ns, f.QuadPart);
        io->out[0] = (unsigned long long)(ns);
    }
    finish(io);
    return (void*)(ULONG_PTR)ok;
}

// ---------------------------------------------------------------- (c) memory

static const unsigned char g_ret42[6] = { 0xB8, 42, 0, 0, 0 /* mov eax,42 */, 0xC3 /* ret */ };

static void query(const char* what, void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T r = VirtualQuery(p, &mbi, sizeof(mbi));
    if (!r) { logf("  VirtualQuery(%s %p) failed GLE=%lu\n", what, p, GetLastError()); return; }
    logf("  VirtualQuery(%s %p): base=%p allocbase=%p size=0x%zx state=0x%lx protect=0x%lx allocprotect=0x%lx type=0x%lx\n",
         what, p, mbi.BaseAddress, mbi.AllocationBase, mbi.RegionSize, mbi.State, mbi.Protect, mbi.AllocationProtect, mbi.Type);
}

static int g_exc_code;
static int filt(unsigned int code) { g_exc_code = (int)code; return EXCEPTION_EXECUTE_HANDLER; }

// try to run the two instructions at p (copied there by the caller), via the asm jump (no CFG check)
static void try_exec(const char* what, void* p)
{
    long long r = -1;
    logf("  exec %s at %p: trying...\n", what, p);
    g_exc_code = 0;
    __try {
        r = probe_jump(p, 0, 0);
        logf("  exec %s at %p: returned %lld %s\n", what, p, r, r == 42 ? "(RAN)" : "");
    } __except (filt(GetExceptionCode())) {
        logf("  exec %s at %p: exception 0x%08x\n", what, p, g_exc_code);
    }
}

static void try_write(const char* what, void* p)
{
    g_exc_code = 0;
    __try {
        memcpy(p, g_ret42, sizeof(g_ret42));
        logf("  write %s at %p: ok\n", what, p);
    } __except (filt(GetExceptionCode())) {
        logf("  write %s at %p: exception 0x%08x\n", what, p, g_exc_code);
    }
}

static const char* protname(DWORD p)
{
    switch (p) {
    case PAGE_NOACCESS: return "NOACCESS";
    case PAGE_READONLY: return "R";
    case PAGE_READWRITE: return "RW";
    case PAGE_EXECUTE: return "X";
    case PAGE_EXECUTE_READ: return "RX";
    case PAGE_EXECUTE_READWRITE: return "RWX";
    case PAGE_EXECUTE_WRITECOPY: return "RWXC";
    case PAGE_WRITECOPY: return "WC";
    default: return "?";
    }
}

static void sync_icache(void* p, size_t n)
{
    // x86_64 keeps instruction and data caches coherent; nothing to do (kept for the AArch64 layout)
    (void)p; (void)n;
}

// args[0] = enclave base (from CreateEnclave), args[1] = enclave size
void* CALLBACK E_Mem(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    begin();
    char* ebase = (char*)io->args[0];
    SIZE_T esize = (SIZE_T)io->args[1];
    unsigned long long phases = io->args[2] ? io->args[2] : ~0ull;
    logf("mem: enclave base=%p size=0x%zx image base=%p gx_code_region=%p E_Mem=%p g_log=%p\n",
         ebase, esize, &__ImageBase, gx_code_region, (void*)E_Mem, g_log);
    if (phases & 1) {
    query("image", &__ImageBase);
    query("text", (void*)E_Mem);
    query("gxcode", gx_code_region);
    query("data", g_log);
    query("stack", &param);
    query("enclave base", ebase);
    // walk the enclave range in 4 KiB steps; print each change of (state, protect, type). VirtualQuery inside
    // the enclave returns BaseAddress=0 for unused enclave pages, so its RegionSize cannot be used to step.
    logf("  regions of the enclave range (4 KiB steps):\n");
    {
        DWORD ls = 0xffffffff, lp = 0, lt = 0; char* start = ebase; void* lb = 0;
        for (char* p = ebase; p <= ebase + esize; p += 4096) {
            MEMORY_BASIC_INFORMATION mbi = { 0 };
            SIZE_T q = (p < ebase + esize) ? VirtualQuery(p, &mbi, sizeof(mbi)) : 0;
            DWORD s2 = q ? mbi.State : 0xfffffffe, p2 = q ? mbi.Protect : 0, t2 = q ? mbi.Type : 0;
            if (p == ebase + esize || s2 != ls || p2 != lp || t2 != lt) {
                if (ls != 0xffffffff)
                    logf("   +0x%08zx..+0x%08zx state=0x%05lx protect=0x%03lx(%s) type=0x%lx (VirtualQuery base of first page %p)\n",
                         (size_t)(start - ebase), (size_t)(p - ebase), ls, lp, protname(lp), lt, lb);
                ls = s2; lp = p2; lt = t2; start = p; lb = q ? mbi.BaseAddress : 0;
            }
        }
    }

    // dynamic allocation with each protection
    DWORD prots[] = { PAGE_READWRITE, PAGE_READONLY, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE };
    for (int i = 0; i < 5; i++) {
        void* a = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, prots[i]);
        logf("VirtualAlloc(NULL, 64K, COMMIT|RESERVE, %s) -> %p GLE=%lu %s\n", protname(prots[i]), a, a ? 0 : GetLastError(),
             a ? (((char*)a >= ebase && (char*)a < ebase + esize) ? "(inside enclave range)" : "(OUTSIDE enclave range)") : "");
        if (a) {
            query("alloc", a);
            VirtualFree(a, 0, MEM_RELEASE);
        }
    }
    // reserve then commit
    void* r = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE, PAGE_READWRITE);
    logf("VirtualAlloc(NULL, 1M, RESERVE, RW) -> %p GLE=%lu\n", r, r ? 0 : GetLastError());
    if (r) {
        void* c = VirtualAlloc(r, 65536, MEM_COMMIT, PAGE_READWRITE);
        logf("VirtualAlloc(reserved, 64K, COMMIT, RW) -> %p GLE=%lu\n", c, c ? 0 : GetLastError());
        BOOL ok = VirtualFree(r, 65536, MEM_DECOMMIT);
        logf("VirtualFree(DECOMMIT) -> %d GLE=%lu\n", ok, ok ? 0 : GetLastError());
        VirtualFree(r, 0, MEM_RELEASE);
    }

    }
    // RW -> RX on dynamic memory
    void* a = (phases & 6) ? VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    if (a) {
        memcpy(a, g_ret42, sizeof(g_ret42));
        if (phases & 2) try_exec("dynamic RW (no X)", a);
        DWORD old = 0;
        DWORD targets[] = { PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE, PAGE_READONLY };
        for (int i = 0; i < 4; i++) {
            BOOL ok = VirtualProtect(a, 4096, targets[i], &old);
            logf("VirtualProtect(dynamic, 4K, %s) -> %d GLE=%lu old=%s\n", protname(targets[i]), ok, ok ? 0 : GetLastError(), protname(old));
            if (ok && (targets[i] & 0xF0)) {
                sync_icache(a, 64);
                try_exec("dynamic after VirtualProtect", a);
            }
            VirtualProtect(a, 4096, PAGE_READWRITE, &old);
        }
        VirtualFree(a, 0, MEM_RELEASE);
    }

    DWORD old = 0;
    if (phases & 8) {
    // the prepared executable region in the signed image
    logf("gxcode first word before = 0x%08x (int3 bytes are 0xcc)\n", *(unsigned int*)gx_code_region);
    if (phases & 0x100) try_write("gxcode (RX image section) without VirtualProtect", gx_code_region);
    DWORD targets2[] = { PAGE_READWRITE, PAGE_EXECUTE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE_WRITECOPY };
    for (int i = 0; i < 4; i++) {
        BOOL ok = VirtualProtect(gx_code_region, 4096, targets2[i], &old);
        logf("VirtualProtect(gxcode, 4K, %s) -> %d GLE=%lu old=%s\n", protname(targets2[i]), ok, ok ? 0 : GetLastError(), protname(old));
        query("gxcode after VirtualProtect", gx_code_region);
        if (ok) {
            try_write("gxcode after VirtualProtect", gx_code_region);
            BOOL ok2 = VirtualProtect(gx_code_region, 4096, PAGE_EXECUTE_READ, &old);
            logf("VirtualProtect(gxcode, 4K, RX) -> %d GLE=%lu old=%s\n", ok2, ok2 ? 0 : GetLastError(), protname(old));
            sync_icache(gx_code_region, 64);
            query("gxcode before exec", gx_code_region);
            if (phases & 0x200) try_exec("gxcode", gx_code_region);
        }
    }
    }
    if (phases & 16) {
    // data section page -> RX
    static __declspec(align(4096)) unsigned char data_page[8192];
    unsigned char* dp = (unsigned char*)(((ULONG_PTR)data_page + 4095) & ~(ULONG_PTR)4095);
    memcpy(dp, g_ret42, sizeof(g_ret42));
    BOOL ok = VirtualProtect(dp, 4096, PAGE_EXECUTE_READ, &old);
    logf("VirtualProtect(.data page, 4K, RX) -> %d GLE=%lu old=%s\n", ok, ok ? 0 : GetLastError(), protname(old));
    if (ok) { sync_icache(dp, 64); try_exec(".data page after VirtualProtect", dp); }
    // the image's own text -> RW
    // an image code page that is not executed (the last page of gxcode)
    void* tp = gx_code_region + 0xF000;
    ok = VirtualProtect(tp, 4096, PAGE_EXECUTE_READWRITE, &old);
    logf("VirtualProtect(image RX page, 4K, RWX) -> %d GLE=%lu old=%s\n", ok, ok ? 0 : GetLastError(), protname(old));
    ok = VirtualProtect(tp, 4096, PAGE_READWRITE, &old);
    logf("VirtualProtect(image RX page, 4K, RW) -> %d GLE=%lu old=%s\n", ok, ok ? 0 : GetLastError(), protname(old));
    query("image RX page after", tp);
    ok = VirtualProtect(tp, 4096, PAGE_EXECUTE_READ, &old);
    logf("VirtualProtect(same page back to RX) -> %d GLE=%lu old=%s\n", ok, ok ? 0 : GetLastError(), protname(old));
    query("image RX page after", tp);
    ok = VirtualProtect(tp, 4096, PAGE_READONLY, &old);
    logf("VirtualProtect(same page R) -> %d GLE=%lu old=%s\n", ok, ok ? 0 : GetLastError(), protname(old));
    ok = VirtualProtect(tp, 4096, PAGE_EXECUTE_READ, &old);
    logf("VirtualProtect(same page back to RX) -> %d GLE=%lu old=%s\n", ok, ok ? 0 : GetLastError(), protname(old));
    query("image RX page after", tp);

    }
    if (phases & 64) {
        volatile char* u = ebase + 0x8000000;
        logf("read of an unused page inside the enclave range %p: trying...\n", u);
        __try { char c = *u; logf("  read ok: %d\n", c); }
        __except (filt(GetExceptionCode())) { logf("  exception 0x%08x\n", g_exc_code); }
    }
    if (phases & 128) {
        char* r2 = (char*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
        logf("write to a dynamic PAGE_READONLY page %p: trying...\n", r2);
        __try { r2[0] = 1; logf("  write ok\n"); }
        __except (filt(GetExceptionCode())) { logf("  exception 0x%08x\n", g_exc_code); }
    }
    if (phases & 32) {
    // how much can be committed dynamically (1 MiB steps, up to 4 GiB)
    static void* blocks[4096];
    int nb = 0;
    for (; nb < 4096; nb++) {
        blocks[nb] = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!blocks[nb]) break;
    }
    DWORD gle = GetLastError();
    logf("dynamic commit: %d MiB in 1 MiB blocks before failure (GLE=%lu); first=%p last=%p\n", nb, gle, nb ? blocks[0] : 0, nb ? blocks[nb - 1] : 0);
    if (nb) {
        // touch every page of the first and last block
        for (int k = 0; k < (1 << 20); k += 4096) { ((char*)blocks[0])[k] = 1; ((char*)blocks[nb - 1])[k] = 1; }
    }
    for (int k = 0; k < nb; k++) VirtualFree(blocks[k], 0, MEM_RELEASE);
    io->out[0] = nb;
    // malloc
    void* m = malloc(64 << 20);
    logf("malloc(64 MiB) -> %p\n", m);
    free(m);
    }
    finish(io);
    return NULL;
}

#ifdef PROBE_RWX
extern unsigned int gx_rwx_region[1024];
void* CALLBACK E_Rwx(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    begin();
    query("gxrwx", gx_rwx_region);
    logf("gxrwx first words 0x%08x 0x%08x\n", gx_rwx_region[0], gx_rwx_region[1]);
    if (io->args[0] & 1) try_exec("gxrwx as loaded", gx_rwx_region);
    if (io->args[0] & 2) { try_write("gxrwx", gx_rwx_region + 16); }
    finish(io);
    return NULL;
}
#endif

// ---------------------------------------------------------------- (d) execution environment

static CONTEXT g_ctx;
static unsigned long long g_filter_sp;
static int g_resume;
static int g_skip;   // bytes to skip when resuming

static int filt_ctx(EXCEPTION_POINTERS* ep)
{
    g_exc_code = (int)ep->ExceptionRecord->ExceptionCode;
    g_ctx = *ep->ContextRecord;
    g_filter_sp = probe_read_rsp();
    if (g_resume) {
        ep->ContextRecord->Rip += g_skip;   // skip the faulting instruction
        ep->ContextRecord->Rax = 0x1234;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

typedef long long (*TESTFN)(void);

static void trap(const char* what, TESTFN fn, int resume, int skip)
{
    long long r = -1;
    g_exc_code = 0;
    g_resume = resume;
    g_skip = skip;
    unsigned long long sp = probe_read_rsp();
    __try {
        r = fn();
        logf("  %s: returned 0x%llx (exception code 0x%08x, resumed=%d)\n", what, r, g_exc_code, resume);
    } __except (filt_ctx(GetExceptionInformation())) {
        logf("  %s: exception 0x%08x at rip=%p sp(fault)=%p sp(filter)=%p sp(caller)=%p rax=0x%llx rcx=0x%llx r11=0x%llx\n",
             what, g_exc_code, (void*)g_ctx.Rip, (void*)g_ctx.Rsp, (void*)g_filter_sp, (void*)sp, g_ctx.Rax, g_ctx.Rcx, g_ctx.R11);
    }
    if (resume && g_exc_code)
        logf("    filter: fault rip=%p (function at %p) fault sp=%p filter sp=%p: filter ran %lld bytes below the faulting sp (%s)\n",
             (void*)g_ctx.Rip, (void*)fn, (void*)g_ctx.Rsp, (void*)g_filter_sp, (long long)(g_ctx.Rsp - g_filter_sp),
             g_filter_sp < g_ctx.Rsp ? "same stack" : "not below");
}

static long long t_int3(void) { return probe_int3(); }
static long long t_ud2(void) { return probe_ud2(); }
static long long t_div0(void) { return probe_div0(); }
static long long t_hlt(void) { return probe_hlt(); }
static long long t_null(void) { return *(volatile long long*)0; }
static long long t_raise(void) { RaiseException(0xE0000001, 0, 0, NULL); return 5; }
static long long t_rdfs(void) { return (long long)probe_rdfsbase(); }
static long long t_rdgs(void) { return (long long)probe_rdgsbase(); }
static long long t_xgetbv0(void) { return (long long)probe_xgetbv(0); }
static long long t_xgetbv1(void) { return (long long)probe_xgetbv(1); }
static long long t_rdtsc(void) { return (long long)__rdtsc(); }
static long long t_rdtscp(void) { unsigned a; return (long long)probe_rdtscp(&a); }
static long long t_rdrand(void) { int ok = 0; unsigned long long v = probe_rdrand(&ok); return ok ? (long long)v : -2; }
static long long t_rdseed(void) { int ok = 0; unsigned long long v = probe_rdseed(&ok); return ok ? (long long)v : -2; }
static long long t_redzone(void) { return probe_redzone_ud2(); }
static char g_buf[] = "hello from syscall\n";
static long long t_sys_write(void) { return probe_syscall(1 /* Linux write */, 1, (long long)g_buf, sizeof(g_buf) - 1); }
static long long t_sys_getpid(void) { return probe_syscall(39 /* Linux getpid */, 0, 0, 0); }
static long long t_int80(void) { return probe_int80(20 /* i386 getpid */); }

// language-specific handler named by probe_fault_offstack's unwind data (as pack.py's static .pdata would)
static volatile LONG g_lh_calls;
static unsigned long long g_lh_rsp, g_lh_frame;
static DWORD g_lh_code;
EXCEPTION_DISPOSITION probe_lang_handler(PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx, PVOID dc)
{
    (void)dc;
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    InterlockedIncrement(&g_lh_calls);
    g_lh_code = rec->ExceptionCode;
    g_lh_rsp = ctx->Rsp;
    g_lh_frame = (unsigned long long)frame;
    ctx->Rip += 2;   // ud2
    return ExceptionContinueExecution;
}

static const unsigned g_leaves[][2] = {
    { 0, 0 }, { 1, 0 }, { 6, 0 }, { 7, 0 }, { 7, 1 }, { 0xb, 0 }, { 0xd, 0 }, { 0xd, 1 }, { 0x14, 0 }, { 0x19, 0 },
    { 0x40000000, 0 }, { 0x40000001, 0 }, { 0x40000003, 0 }, { 0x80000000, 0 }, { 0x80000001, 0 }, { 0x80000007, 0 },
    { 0x80000008, 0 },
};

static void cpuid_all(void)
{
    for (int i = 0; i < (int)(sizeof(g_leaves) / sizeof(g_leaves[0])); i++) {
        unsigned r[4] = { 0 };
        g_exc_code = 0;
        __try {
            probe_cpuid(g_leaves[i][0], g_leaves[i][1], r);
            logf("  cpuid(0x%08x,%u): eax=%08x ebx=%08x ecx=%08x edx=%08x\n", g_leaves[i][0], g_leaves[i][1], r[0], r[1], r[2], r[3]);
        } __except (filt(GetExceptionCode())) {
            logf("  cpuid(0x%08x,%u): exception 0x%08x\n", g_leaves[i][0], g_leaves[i][1], g_exc_code);
        }
    }
}

static void stack_info(const char* when)
{
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    logf("%s: TEB=%p StackBase=%p StackLimit=%p (%llu KiB) rsp=%p ArbitraryUserPointer=%p\n", when, (void*)tib,
         tib->StackBase, tib->StackLimit, (unsigned long long)((char*)tib->StackBase - (char*)tib->StackLimit) / 1024,
         (void*)probe_read_rsp(), tib->ArbitraryUserPointer);
}

static __declspec(noinline) long long recurse_kib(int n)
{
    volatile char buf[1000];
    buf[0] = (char)n;
    buf[999] = (char)n;
    if (n <= 1) return buf[0] + buf[999];
    return recurse_kib(n - 1) + buf[0] - buf[999] + 1;
}

// args[0]: group. 1 = safe probes, 2 = syscall, 3 = trap+resume, 4 = FS base kept across exit/entry,
// 5 = red zone, 6 = FS base and registers across OCALLs, 7 = FS base and registers under preemption,
// 8 = exceptions with rsp outside the thread stack, 9 = stack, 10 = CFG, 11 = TEB layout, 12 = stack growth, 13 = exception cost.
void* CALLBACK E_Exec(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    begin();
    switch (io->args[0]) {
    case 1: {
        stack_info("exec");
        logf("exec: NtCurrentTeb()=%p gs:[0x30]=0x%llx\n", (void*)NtCurrentTeb(), __readgsqword(0x30));
        // TEB fields that Gramine could borrow on x64 (see PLAN-x64.md): NT_TIB.ExceptionList (0x0),
        // SubSystemTib (0x18, where the patched glibc's `jmp *%gs:24` would look), FiberData (0x20),
        // ArbitraryUserPointer (0x28), ThreadLocalStoragePointer (0x58), TlsSlots (0x1480)
        logf("exec: TEB fields: [0x0]=0x%llx [0x18]=0x%llx [0x20]=0x%llx [0x28]=0x%llx [0x58]=0x%llx\n",
             __readgsqword(0x0), __readgsqword(0x18), __readgsqword(0x20), __readgsqword(0x28), __readgsqword(0x58));
        {
            DWORD idx[4];
            for (int k = 0; k < 4; k++) idx[k] = TlsAlloc();
            TlsSetValue(idx[0], (void*)0x1234);
            logf("exec: TlsAlloc x4 -> %lu %lu %lu %lu; after TlsSetValue(first, 0x1234): gs:[0x1480+8*first]=0x%llx\n",
                 idx[0], idx[1], idx[2], idx[3], idx[0] < 64 ? __readgsqword(0x1480 + 8 * idx[0]) : 0ull);
            for (int k = 0; k < 4; k++) TlsFree(idx[k]);
        }
        trap("rdfsbase on entry", t_rdfs, 0, 0);
        trap("rdgsbase on entry", t_rdgs, 0, 0);
        trap("xgetbv(0) (XCR0)", t_xgetbv0, 0, 0);
        trap("xgetbv(1) (XINUSE)", t_xgetbv1, 0, 0);
        trap("rdtsc", t_rdtsc, 0, 0);
        trap("rdtscp", t_rdtscp, 0, 0);
        trap("rdrand", t_rdrand, 0, 0);
        trap("rdseed", t_rdseed, 0, 0);
        logf("exec: MXCSR=0x%x EFLAGS=0x%llx\n", _mm_getcsr(), (unsigned long long)__readeflags());
        logf("exec: cpuid inside the enclave:\n");
        cpuid_all();
        // FS base: write a canonical user address, spin 200 ms (the thread will be preempted), read again
        g_exc_code = 0;
        __try {
            unsigned long long v = 0x00007ff011223340ull;
            probe_wrfsbase(v);
            unsigned long long t1 = probe_rdfsbase();
            LARGE_INTEGER f, a, b, prev;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            prev = a;
            long long maxgap = 0;
            do { QueryPerformanceCounter(&b); if (b.QuadPart - prev.QuadPart > maxgap) maxgap = b.QuadPart - prev.QuadPart; prev = b; }
            while ((b.QuadPart - a.QuadPart) < f.QuadPart / 5);
            unsigned long long t2 = probe_rdfsbase();
            probe_wrfsbase(0);
            logf("exec: wrfsbase(0x%llx) reads 0x%llx; after a 200 ms spin with QPC calls (max QPC gap %lld us) reads 0x%llx\n",
                 v, t1, maxgap * 1000000 / f.QuadPart, t2);
            io->out[0] = t2;
        } __except (filt(GetExceptionCode())) {
            logf("exec: wrfsbase/rdfsbase: exception 0x%08x\n", g_exc_code);
        }
        for (int pf = 0; pf < 96; pf++) if (IsProcessorFeaturePresent(pf)) logf("PF%d ", pf);
        logf("\n(IsProcessorFeaturePresent inside the enclave: the numbers present)\n");
        logf("exec: GetEnabledXStateFeatures()=0x%llx\n", (unsigned long long)GetEnabledXStateFeatures());
        trap("RaiseException(0xE0000001)", t_raise, 0, 0);
        trap("load from NULL", t_null, 0, 0);
        break;
    }
    case 2:
        logf("exec: syscall with rax=39 (Linux getpid):\n");
        finish(io);
        trap("syscall getpid", t_sys_getpid, 0, 0);
        logf("    after syscall: rcx=0x%llx r11=0x%llx (set to 0xc0c0.. and 0xb1b1.. before)\n", g_after_syscall_rcx, g_after_syscall_r11);
        logf("exec: syscall with rax=1 (Linux write(1, buf, 19)):\n");
        finish(io);
        trap("syscall write", t_sys_write, 0, 0);
        logf("    after syscall: rcx=0x%llx r11=0x%llx\n", g_after_syscall_rcx, g_after_syscall_r11);
        logf("exec: int 0x80 with eax=20:\n");
        finish(io);
        trap("int 0x80", t_int80, 0, 0);
        break;
    case 3:
        trap("int3, catch", t_int3, 0, 0);
        trap("ud2, catch", t_ud2, 0, 0);
        trap("div by zero, catch", t_div0, 0, 0);
        trap("hlt, catch", t_hlt, 0, 0);
        trap("int3, resume at rip+1 with rax=0x1234", t_int3, 1, 1);
        trap("ud2, resume at rip+2 with rax=0x1234", t_ud2, 1, 2);
        trap("div by zero, resume at rip+2 with rax=0x1234", t_div0, 1, 2);
        trap("hlt, resume at rip+1 with rax=0x1234", t_hlt, 1, 1);
        break;
    case 4:
        // FS base persistence across a return to the host and a new entry
        __try {
            io->out[0] = probe_rdfsbase();
            if (io->args[1]) probe_wrfsbase(io->args[1]);
            io->out[1] = probe_rdgsbase();
            logf("exec: FS base on entry 0x%llx, set to 0x%llx; GS base 0x%llx, TEB %p\n", io->out[0], io->args[1], io->out[1], (void*)NtCurrentTeb());
        } __except (filt(GetExceptionCode())) {
            logf("exec: rdfsbase/wrfsbase: exception 0x%08x\n", g_exc_code);
            io->out[2] = g_exc_code;
        }
        break;
    case 5: {
        // red zone: does the exception dispatch write into the 128 bytes below the faulting rsp?
        trap("red zone pattern, ud2, resume at rip+2", t_redzone, 1, 2);
        logf("red zone: (the value returned above is a bit mask of the 16 qwords below rsp that changed: bit 16 = rsp-8, bit 1 = rsp-128)\n");
        unsigned long long iters = io->args[1] ? io->args[1] : 2000000000ull;
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        long long changed = probe_redzone_spin(iters);
        QueryPerformanceCounter(&b);
        logf("red zone: pattern below rsp kept through a spin of %llu iterations (%lld ms, no calls): changed mask 0x%llx\n",
             iters, (b.QuadPart - a.QuadPart) * 1000 / f.QuadPart, changed);
        io->out[0] = (unsigned long long)changed;
        break;
    }
    case 6: {
        // FS base, GS base, ArbitraryUserPointer and callee-saved registers across OCALLs
        LPENCLAVE_ROUTINE fn = (LPENCLAVE_ROUTINE)io->args[1];
        unsigned long long n = io->args[2] ? io->args[2] : 100000;
        unsigned long long fs_bad = 0, gs_bad = 0, aup_bad = 0, first_fs = 0, calls_bad = 0, fs_nz = 0, first_nz = 0;
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        void* aup0 = tib->ArbitraryUserPointer;
        void* sst0 = tib->SubSystemTib;
        unsigned long long gs0 = 0;
        int have_fsgs = 1;
        __try { gs0 = probe_rdgsbase(); } __except (filt(GetExceptionCode())) { have_fsgs = 0; }
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        for (unsigned long long i = 0; i < n; i++) {
            unsigned long long v = 0x00007ee200000000ull + (i << 4);
            if (have_fsgs) probe_wrfsbase(v);
            tib->ArbitraryUserPointer = (void*)(v + 8);
            tib->SubSystemTib = (void*)(v + 16);
            void* ret = NULL;
            if (!CallEnclave(fn, (void*)i, TRUE, &ret) || ret != (void*)(i + 1)) calls_bad++;
            if (have_fsgs) {
                unsigned long long g = probe_rdfsbase();
                if (g != v) { if (!fs_bad) first_fs = g; fs_bad++; if (g) { if (!fs_nz) first_nz = g; fs_nz++; } }
                if (probe_rdgsbase() != gs0) gs_bad++;
            }
            if (tib->ArbitraryUserPointer != (void*)(v + 8) || tib->SubSystemTib != (void*)(v + 16)) aup_bad++;
        }
        QueryPerformanceCounter(&b);
        if (have_fsgs) probe_wrfsbase(0);
        tib->ArbitraryUserPointer = aup0;
        tib->SubSystemTib = sst0;
        logf("ocall: %llu OCALLs (%llu failed) in %lld ms; FS base changed %llu times (first value 0x%llx); GS base changed %llu times; "
             "TEB ArbitraryUserPointer or SubSystemTib changed %llu times%s; FS base changed to a NON-ZERO value %llu times (first 0x%llx)\n",
             n, calls_bad, (b.QuadPart - a.QuadPart) * 1000 / f.QuadPart,
             fs_bad, first_fs, gs_bad, aup_bad, have_fsgs ? "" : " (rd/wrfsbase unavailable: FS not tested)", fs_nz, first_nz);
        long long m = probe_callout_regs((void*)fn);
        logf("ocall: callee-saved registers changed across CallEnclave to the host: mask 0x%llx (0 = rbx rsi rdi rbp r12-r15 xmm6-15 kept)\n", m);
        io->out[0] = fs_bad; io->out[1] = gs_bad; io->out[2] = aup_bad; io->out[3] = (unsigned long long)m;
        break;
    }
    case 7: {
        // FS base and registers under preemption (no calls); run several processes at once for contention
        unsigned long long iters = io->args[1] ? io->args[1] : 4000000000ull;
        unsigned long long fo[3] = { 0 };
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        long long fs_changes = -1;
        __try { fs_changes = probe_fs_spin(0x00007ee300000010ull, iters, fo); }
        __except (filt(GetExceptionCode())) { logf("spin: rdfsbase/wrfsbase exception 0x%08x\n", g_exc_code); }
        QueryPerformanceCounter(&b);
        long long ms1 = (b.QuadPart - a.QuadPart) * 1000 / f.QuadPart;
        QueryPerformanceCounter(&a);
        long long regs = probe_regs_spin(iters / 2);
        QueryPerformanceCounter(&b);
        long long ms2 = (b.QuadPart - a.QuadPart) * 1000 / f.QuadPart;
        logf("spin: FS base compared on each of %llu iterations (%lld ms): %lld changes, first different value 0x%llx; "
             "%llu changes to a NON-ZERO value, first 0x%llx\n", iters, ms1, fs_changes, fo[0], fo[2], fo[1]);
        logf("spin: registers after %llu iterations (%lld ms): changed mask 0x%llx (0 = rbx rsi rdi rbp r8-r15 xmm6-15 ymm0-5 kept)\n", iters / 2, ms2, regs);
        io->out[0] = (unsigned long long)fs_changes; io->out[1] = fo[2]; io->out[2] = (unsigned long long)regs;
        break;
    }
    case 8: {
        // an exception with rsp in VirtualAlloc'd memory outside the thread's stack
        int variant = (int)io->args[1];
        char* st = (char*)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        void* top = st + 65536 - 64;
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        void* sb = tib->StackBase; void* sl = tib->StackLimit;
        logf("offstack: variant %d, new stack %p..%p, thread stack %p..%p\n", variant, st, st + 65536, sl, sb);
        finish(io);
        long long r = -1;
        g_exc_code = 0;
        __try {
            if (variant == 2) { tib->StackLimit = st; tib->StackBase = st + 65536; }
            if (variant == 3) r = probe_fault_offstack_nohandler(top);
            else r = probe_fault_offstack(top);
            tib->StackLimit = sl; tib->StackBase = sb;
            logf("offstack: returned %lld; language handler called %ld times, code 0x%08lx, rsp %p, establisher frame %p\n",
                 r, g_lh_calls, g_lh_code, (void*)g_lh_rsp, (void*)g_lh_frame);
        } __except (filt(GetExceptionCode())) {
            tib->StackLimit = sl; tib->StackBase = sb;
            logf("offstack: caught by the C filter: exception 0x%08x (language handler called %ld times)\n", g_exc_code, g_lh_calls);
        }
        io->out[0] = (unsigned long long)r; io->out[1] = g_lh_calls;
        break;
    }
    case 9: {
        stack_info("stack");
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        query("StackLimit", tib->StackLimit);
        query("StackLimit-4K", (char*)tib->StackLimit - 4096);
        query("StackLimit-64K", (char*)tib->StackLimit - 65536);
        query("StackBase-4K", (char*)tib->StackBase - 4096);
        io->out[0] = (unsigned long long)((char*)tib->StackBase - (char*)tib->StackLimit);
        break;
    }
    case 12: {
        // stack growth: recurse args[1] KiB deep (1 KiB per frame)
        unsigned long long kib = io->args[1] ? io->args[1] : 256;
        g_exc_code = 0;
        long long r = -1;
        __try { r = recurse_kib((int)kib); }
        __except (filt(GetExceptionCode())) { }
        stack_info("after recursion");
        logf("stack: recursion %llu KiB deep -> %lld, exception 0x%08x\n", kib, r, g_exc_code);
        io->out[0] = (unsigned long long)r;
        break;
    }
    case 13: {
        // cost of an exception handled by the unwind-data handler and resumed (the path packed code
        // would take): args[1] iterations of probe_fault_offstack with widened TEB bounds
        unsigned long long n = io->args[1] ? io->args[1] : 10000;
        char* st = (char*)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        void* sb = tib->StackBase; void* sl = tib->StackLimit;
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        g_lh_calls = 0;
        tib->StackLimit = st; tib->StackBase = st + 65536;
        QueryPerformanceCounter(&a);
        for (unsigned long long i = 0; i < n; i++) probe_fault_offstack(st + 65536 - 64);
        QueryPerformanceCounter(&b);
        tib->StackLimit = sl; tib->StackBase = sb;
        double ns = (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart / (double)n;
        logf("exccost: %llu exceptions (ud2, unwind-data handler, resume) in the enclave: %.0f ns each; handler calls %ld\n", n, ns, g_lh_calls);
        io->out[0] = (unsigned long long)ns;
        break;
    }
    case 11: {
        // the enclave thread's TEB: how large is it, and which of its qwords are in use (non-zero)?
        // (Gramine's own TCB could live in an unused part of it, see PLAN-x64.md, X2)
        unsigned char* teb = (unsigned char*)NtCurrentTeb();
        query("TEB", teb);
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T avail = 0;
        if (VirtualQuery(teb, &mbi, sizeof(mbi))) avail = (SIZE_T)((unsigned char*)mbi.BaseAddress + mbi.RegionSize - teb);
        if (avail > 0x2000) avail = 0x2000;
        logf("teb: %p, %zu bytes readable from it in its region; non-zero qwords (offset=value):\n", teb, avail);
        int n = 0;
        for (SIZE_T off = 0; off + 8 <= avail; off += 8) {
            unsigned long long v = *(unsigned long long*)(teb + off);
            if (v) { logf(" %03zx=%llx", off, v); if (++n % 6 == 0) logf("\n"); }
        }
        logf("\nteb: %d non-zero qwords\n", n);
        io->out[0] = avail; io->out[1] = n;
        break;
    }
    case 10: {
        // CFG: an indirect C call (compiled with /guard:cf) to an address that is not a CFG target
        // (inside probe_int3, after the int3: mov eax,7; ret)
        TESTFN fn = (TESTFN)((char*)probe_int3 + 1);
        logf("cfg: indirect call to %p (not a function start)...\n", (void*)fn);
        finish(io);
        long long r = fn();
        logf("cfg: returned %lld: CFG did not stop the call\n", r);
        break;
    }
    }
    finish(io);
    return NULL;
}

// ---------------------------------------------------------------- (e) threads

static volatile LONG g_word;
static volatile LONG g_inside;

// args[0] = 1: wait on g_word (timeout args[1] ms); 2: wake; 3: spin args[1] ms and report max gap; 4: block forever-ish (args[1] ms) counting
void* CALLBACK E_Thread(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    LARGE_INTEGER f, a, b, prev;
    QueryPerformanceFrequency(&f);
    InterlockedIncrement(&g_inside);
    switch (io->args[0]) {
    case 1: {
        LONG undesired = 0;
        QueryPerformanceCounter(&a);
        BOOL ok = TRUE;
        while (g_word == 0 && ok) ok = WaitOnAddress(&g_word, &undesired, sizeof(LONG), (DWORD)io->args[1]);
        QueryPerformanceCounter(&b);
        io->out[0] = ok;
        io->out[1] = ok ? 0 : GetLastError();
        io->out[2] = (b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart;
        io->out[3] = GetCurrentThreadId();
        break;
    }
    case 2:
        InterlockedExchange(&g_word, 1);
        WakeByAddressAll((PVOID)&g_word);
        io->out[3] = GetCurrentThreadId();
        break;
    case 3: {
        QueryPerformanceCounter(&a);
        prev = a;
        long long maxgap = 0, n = 0;
        do { QueryPerformanceCounter(&b); n++; if (b.QuadPart - prev.QuadPart > maxgap) maxgap = b.QuadPart - prev.QuadPart; prev = b; }
        while ((b.QuadPart - a.QuadPart) < (long long)io->args[1] * f.QuadPart / 1000);
        io->out[0] = maxgap * 1000000 / f.QuadPart;
        io->out[1] = n;
        io->out[3] = GetCurrentThreadId();
        break;
    }
    case 4: {
        // TLS slot inside the enclave
        DWORD idx = TlsAlloc();
        io->out[0] = idx;
        io->out[1] = idx == TLS_OUT_OF_INDEXES ? GetLastError() : TlsSetValue(idx, (void*)0x77);
        io->out[2] = (unsigned long long)TlsGetValue(idx);
        if (idx != TLS_OUT_OF_INDEXES) TlsFree(idx);
        io->out[3] = GetCurrentThreadId();
        break;
    }
    }
    InterlockedDecrement(&g_inside);
    return NULL;
}

// ---------------------------------------------------------------- (f) runtime APIs

static void hex(const char* what, const unsigned char* p, size_t n)
{
    logf("  %s (%zu bytes):", what, n);
    for (size_t i = 0; i < n; i++) logf("%s%02x", (i % 32) ? "" : "\n   ", p[i]);
    logf("\n");
}

static void identity(const char* what, const ENCLAVE_IDENTITY* id)
{
    logf(" %s: SVN=%u type=0x%x\n", what, id->EnclaveSvn, id->EnclaveType);
    hex("OwnerId", id->OwnerId, sizeof(id->OwnerId));
    hex("UniqueId", id->UniqueId, sizeof(id->UniqueId));
    hex("AuthorId", id->AuthorId, sizeof(id->AuthorId));
    hex("FamilyId", id->FamilyId, sizeof(id->FamilyId));
    hex("ImageId", id->ImageId, sizeof(id->ImageId));
}

static unsigned char g_report[16384];

void* CALLBACK E_Api(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    begin();
    ENCLAVE_INFORMATION info;
    HRESULT hr = EnclaveGetEnclaveInformation(sizeof(info), &info);
    logf("EnclaveGetEnclaveInformation hr=0x%08lx type=0x%lx Size=0x%llx BaseAddress=%p\n", hr, info.EnclaveType,
         (unsigned long long)info.Size, info.BaseAddress);
    if (SUCCEEDED(hr)) identity("identity", &info.Identity);

    unsigned char rnd[16] = { 0 };
    NTSTATUS st = BCryptGenRandom(NULL, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    logf("BCryptGenRandom status=0x%08lx\n", st);
    hex("random", rnd, sizeof(rnd));

    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    logf("QueryPerformanceFrequency=%lld QueryPerformanceCounter=%lld\n", f.QuadPart, c.QuadPart);

    // attestation report
    UINT8 data[ENCLAVE_REPORT_DATA_LENGTH];
    for (int i = 0; i < ENCLAVE_REPORT_DATA_LENGTH; i++) data[i] = (UINT8)i;
    UINT32 sz = 0;
    hr = EnclaveGetAttestationReport(data, NULL, 0, &sz);
    logf("EnclaveGetAttestationReport(size query) hr=0x%08lx size=%u\n", hr, sz);
    hr = EnclaveGetAttestationReport(data, g_report, sizeof(g_report), &sz);
    logf("EnclaveGetAttestationReport hr=0x%08lx size=%u\n", hr, sz);
    if (SUCCEEDED(hr) && sz >= sizeof(VBS_ENCLAVE_REPORT_PKG_HEADER)) {
        VBS_ENCLAVE_REPORT_PKG_HEADER* h = (VBS_ENCLAVE_REPORT_PKG_HEADER*)g_report;
        logf(" pkg header: PackageSize=%u Version=%u SignatureScheme=%u SignedStatementSize=%u SignatureSize=%u\n",
             h->PackageSize, h->Version, h->SignatureScheme, h->SignedStatementSize, h->SignatureSize);
        VBS_ENCLAVE_REPORT* r = (VBS_ENCLAVE_REPORT*)(g_report + sizeof(*h));
        logf(" report: ReportSize=%u ReportVersion=%u EnclaveData[0..3]=%02x%02x%02x%02x\n", r->ReportSize, r->ReportVersion,
             r->EnclaveData[0], r->EnclaveData[1], r->EnclaveData[2], r->EnclaveData[3]);
        identity("report identity", &r->EnclaveIdentity);
        unsigned char* v = (unsigned char*)r + sizeof(VBS_ENCLAVE_REPORT);
        unsigned char* end = (unsigned char*)r + r->ReportSize;
        while (v + sizeof(VBS_ENCLAVE_REPORT_VARDATA_HEADER) <= end) {
            VBS_ENCLAVE_REPORT_VARDATA_HEADER* vh = (VBS_ENCLAVE_REPORT_VARDATA_HEADER*)v;
            if (vh->Size == 0) break;
            if (vh->DataType == VBS_ENCLAVE_VARDATA_MODULE) {
                VBS_ENCLAVE_REPORT_MODULE* m = (VBS_ENCLAVE_REPORT_MODULE*)v;
                logf(" module: %ls svn=%u\n", m->ModuleName, m->Svn);
            } else {
                logf(" vardata type=%u size=%u\n", vh->DataType, vh->Size);
            }
            v += vh->Size;
        }
        hr = EnclaveVerifyAttestationReport(ENCLAVE_TYPE_VBS, g_report, sz);
        logf("EnclaveVerifyAttestationReport(own report) hr=0x%08lx\n", hr);
        g_report[sz - 1] ^= 1;
        hr = EnclaveVerifyAttestationReport(ENCLAVE_TYPE_VBS, g_report, sz);
        logf("EnclaveVerifyAttestationReport(last byte flipped) hr=0x%08lx\n", hr);
        g_report[sz - 1] ^= 1;
        // copy the report out for the host to keep
        if (sz <= sizeof(io->text) / 2) { memcpy(io->text + sizeof(io->text) / 2, g_report, sz); io->out[0] = sz; }
    }

    // sealing
    static const char secret[] = "gramine sealing test";
    unsigned char blob[1024];
    UINT32 bsz = 0;
    ENCLAVE_SEALING_IDENTITY_POLICY pols[] = { ENCLAVE_IDENTITY_POLICY_SEAL_EXACT_CODE, ENCLAVE_IDENTITY_POLICY_SEAL_SAME_PRIMARY_CODE,
                                               ENCLAVE_IDENTITY_POLICY_SEAL_SAME_IMAGE, ENCLAVE_IDENTITY_POLICY_SEAL_SAME_FAMILY,
                                               ENCLAVE_IDENTITY_POLICY_SEAL_SAME_AUTHOR };
    for (int i = 0; i < 5; i++) {
        hr = EnclaveSealData(secret, sizeof(secret), pols[i], ENCLAVE_RUNTIME_POLICY_ALLOW_FULL_DEBUG, blob, sizeof(blob), &bsz);
        logf("EnclaveSealData(policy %d, ALLOW_FULL_DEBUG) hr=0x%08lx blob=%u bytes\n", (int)pols[i], hr, bsz);
        if (SUCCEEDED(hr)) {
            char out[64] = { 0 };
            UINT32 osz = 0, flags = 0;
            ENCLAVE_IDENTITY sid;
            hr = EnclaveUnsealData(blob, bsz, out, sizeof(out), &osz, &sid, &flags);
            logf("  EnclaveUnsealData hr=0x%08lx size=%u flags=0x%x text=\"%s\" %s\n", hr, osz, flags, out,
                 (SUCCEEDED(hr) && !strcmp(out, secret)) ? "(round trip OK)" : "");
            if (i == 2 && bsz <= 1024) { memcpy(io->text + sizeof(io->text) / 2 + 8192, blob, bsz); io->out[1] = bsz; }
        }
    }
    hr = EnclaveSealData(secret, sizeof(secret), ENCLAVE_IDENTITY_POLICY_SEAL_EXACT_CODE, 0, blob, sizeof(blob), &bsz);
    logf("EnclaveSealData(EXACT_CODE, runtime policy 0, in a debug enclave) hr=0x%08lx\n", hr);
    // unseal a blob given by the host (from an earlier run)
    if (io->args[0]) {
        char out[64] = { 0 };
        UINT32 osz = 0, flags = 0;
        hr = EnclaveUnsealData((void*)io->args[0], (UINT32)io->args[1], out, sizeof(out), &osz, NULL, &flags);
        logf("EnclaveUnsealData(blob from an earlier enclave instance) hr=0x%08lx text=\"%s\" flags=0x%x\n", hr, out, flags);
    }
    // heap and TLS
    HANDLE heap = GetProcessHeap();
    void* hp = HeapAlloc(heap, 0, 1 << 20);
    logf("GetProcessHeap=%p HeapAlloc(1 MiB)=%p\n", heap, hp);
    HeapFree(heap, 0, hp);
    OutputDebugStringW(L"probe enclave: OutputDebugStringW\n");
    logf("OutputDebugStringW called (visible only to a debugger)\n");
    finish(io);
    return NULL;
}
