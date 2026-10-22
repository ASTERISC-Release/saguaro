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
// Saguaro phase-0 spike: the probe enclave. The enclave configuration and DLL skeleton come from
// Microsoft's VbsEnclave sample (MIT); the probes are new. Every export takes a PROBE_IO pointer in
// host memory and writes its results there.

#include <winenclave.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
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

// armasm64 helpers (probe.asm)
long long probe_svc0(long long a0, long long a1, long long a2, long long nr);
long long probe_brk0(void);
long long probe_brkf000(void);
long long probe_udf(void);
unsigned long long probe_read_tpidr(void);
void probe_write_tpidr(unsigned long long v);
unsigned long long probe_read_tpidrro(void);
unsigned long long probe_read_x18(void);
unsigned long long probe_read_sp(void);
long long probe_jump(void* fn, long long a0, long long a1);
void probe_sync_icache(void* p, size_t len);
unsigned long long probe_read_isar0(void);
unsigned long long probe_read_midr(void);
unsigned long long probe_read_cntvct(void);
unsigned long long probe_read_cntfrq(void);
long long probe_casal(long long* p, long long expected, long long desired);
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

static const unsigned int g_ret42[2] = { 0xd2800540 /* mov x0,#42 */, 0xd65f03c0 /* ret */ };

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
    __try { probe_sync_icache(p, n); logf("  dc cvau/ic ivau/dsb/isb on %p: ok\n", p); }
    __except (filt(GetExceptionCode())) { logf("  dc cvau/ic ivau on %p: exception 0x%08x\n", p, g_exc_code); }
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
    logf("gxcode first word before = 0x%08x\n", *(unsigned int*)gx_code_region);
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

static int filt_ctx(EXCEPTION_POINTERS* ep)
{
    g_exc_code = (int)ep->ExceptionRecord->ExceptionCode;
    g_ctx = *ep->ContextRecord;
    g_filter_sp = probe_read_sp();
    if (g_resume) {
        ep->ContextRecord->Pc += 4;   // skip the faulting instruction
        ep->ContextRecord->X0 = 0x1234;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

typedef long long (*TESTFN)(void);

static void trap(const char* what, TESTFN fn, int resume)
{
    long long r = -1;
    g_exc_code = 0;
    g_resume = resume;
    unsigned long long sp = probe_read_sp();
    __try {
        r = fn();
        logf("  %s: returned 0x%llx (exception code 0x%08x, resumed=%d)\n", what, r, g_exc_code, resume);
    } __except (filt_ctx(GetExceptionInformation())) {
        logf("  %s: exception 0x%08x at pc=%p sp(fault)=%p sp(filter)=%p sp(caller)=%p x8=0x%llx\n", what, g_exc_code,
             (void*)g_ctx.Pc, (void*)g_ctx.Sp, (void*)g_filter_sp, (void*)sp, g_ctx.X8);
    }
    if (resume && g_exc_code)
        logf("    filter: fault sp=%p filter sp=%p (filter ran %s the faulting frame)\n", (void*)g_ctx.Sp, (void*)g_filter_sp,
             g_filter_sp < g_ctx.Sp ? "below, on the same stack as," : "not below");
}

static long long t_brk0(void) { return probe_brk0(); }
static long long t_brkf000(void) { return probe_brkf000(); }
static long long t_udf(void) { return probe_udf(); }
static long long t_isar0(void) { return (long long)probe_read_isar0(); }
static long long t_midr(void) { return (long long)probe_read_midr(); }
static long long t_null(void) { return *(volatile long long*)0; }
static long long t_raise(void) { RaiseException(0xE0000001, 0, 0, NULL); return 5; }
static char g_buf[] = "hello from svc\n";
static long long t_svc_write(void) { return probe_svc0(1, (long long)g_buf, sizeof(g_buf) - 1, 64 /* Linux write */); }
static long long t_svc_getpid(void) { return probe_svc0(0, 0, 0, 172 /* Linux getpid */); }

// args[0]: which group: 1 = safe probes, 2 = svc, 3 = trap+resume
void* CALLBACK E_Exec(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    begin();
    switch (io->args[0]) {
    case 1: {
        unsigned long long t0 = probe_read_tpidr();
        logf("exec: TPIDR_EL0 on entry = 0x%llx, TPIDRRO_EL0 = 0x%llx, x18 = 0x%llx, NtCurrentTeb() = %p, sp = 0x%llx\n",
             t0, probe_read_tpidrro(), probe_read_x18(), (void*)NtCurrentTeb(), probe_read_sp());
        probe_write_tpidr(0x1122334455667788ull);
        unsigned long long t1 = probe_read_tpidr();
        logf("exec: TPIDR_EL0 after write 0x1122334455667788 reads 0x%llx\n", t1);
        // spin 200 ms with QPC (the thread will be preempted on a busy machine) then read again
        LARGE_INTEGER f, a, b, prev;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        prev = a;
        long long maxgap = 0;
        do { QueryPerformanceCounter(&b); if (b.QuadPart - prev.QuadPart > maxgap) maxgap = b.QuadPart - prev.QuadPart; prev = b; }
        while ((b.QuadPart - a.QuadPart) < f.QuadPart / 5);
        logf("exec: after 200 ms spin (max QPC gap %lld us) TPIDR_EL0 reads 0x%llx\n", maxgap * 1000000 / f.QuadPart, probe_read_tpidr());
        io->out[0] = probe_read_tpidr();
        logf("exec: CNTVCT_EL0=0x%llx CNTFRQ_EL0=%llu QPF=%lld\n", probe_read_cntvct(), probe_read_cntfrq(), f.QuadPart);
        long long v = 5;
        long long old = probe_casal(&v, 5, 6);
        logf("exec: LSE casal: old=%lld new=%lld\n", old, v);
        for (int pf = 0; pf < 96; pf++) if (IsProcessorFeaturePresent(pf)) logf("PF%d ", pf);
        logf("\n(IsProcessorFeaturePresent inside the enclave: the numbers present)\n");
        trap("RaiseException(0xE0000001)", t_raise, 0);
        trap("load from NULL", t_null, 0);
        trap("mrs ID_AA64ISAR0_EL1", t_isar0, 0);
        trap("mrs MIDR_EL1", t_midr, 0);
        break;
    }
    case 2:
        logf("exec: svc #0 with x8=172 (Linux getpid):\n");
        finish(io);
        trap("svc getpid", t_svc_getpid, 0);
        logf("exec: svc #0 with x8=64 (Linux write(1, buf, 15)):\n");
        finish(io);
        trap("svc write", t_svc_write, 0);
        break;
    case 3:
        trap("brk #0xf000 (__debugbreak), catch", t_brkf000, 0);
        trap("brk #0, catch", t_brk0, 0);
        trap("udf #0, catch", t_udf, 0);
        trap("brk #0, resume at pc+4 with x0=0x1234", t_brk0, 1);
        trap("udf #0, resume at pc+4 with x0=0x1234", t_udf, 1);
        break;
    case 4:
        // TPIDR_EL0 persistence across a return to the host and a new entry
        io->out[0] = probe_read_tpidr();
        if (io->args[1]) probe_write_tpidr(io->args[1]);
        logf("exec: TPIDR_EL0 on entry 0x%llx, set to 0x%llx\n", io->out[0], io->args[1]);
        break;
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
