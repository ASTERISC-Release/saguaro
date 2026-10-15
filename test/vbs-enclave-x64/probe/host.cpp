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
// Gramine phase-0 spike: host of the probe enclave. The enclave creation sequence follows
// Microsoft's VbsEnclave sample (MIT). Usage: host.exe <test> [args]; see main().
//
// x64 version (test/vbs-enclave-x64/): tests for the x86_64 probes and a host-only survey (hostcpu).
// The x64 changes: Copyright (C) 2026 The ASTERISC Research Lab

#include <windows.h>
#include <enclaveapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include <immintrin.h>
#include "probe.h"

// probe.asm helpers, linked into the host too for the host-only proxies (hostexc)
extern "C" {
long long probe_int3(void);
long long probe_ud2(void);
long long probe_div0(void);
long long probe_redzone_ud2(void);
long long probe_redzone_spin(unsigned long long iters);
long long probe_regs_spin(unsigned long long iters);
unsigned long long probe_read_rsp(void);
long long probe_fault_offstack(void* new_rsp);
static volatile LONG g_lh_calls;
EXCEPTION_DISPOSITION probe_lang_handler(PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx, PVOID dc)
{
    (void)frame; (void)dc;
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    InterlockedIncrement(&g_lh_calls);
    ctx->Rip += 2;
    return ExceptionContinueExecution;
}
}

static CONTEXT g_hctx;
static unsigned long long g_hfilter_sp;
static int g_hskip;
static int host_filt(EXCEPTION_POINTERS* ep)
{
    g_hctx = *ep->ContextRecord;
    g_hfilter_sp = probe_read_rsp();
    ep->ContextRecord->Rip += g_hskip;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void host_trap(const char* what, long long (*fn)(void), int skip)
{
    g_hskip = skip;
    memset(&g_hctx, 0, sizeof(g_hctx));
    long long r = -1;
    __try { r = fn(); } __except (host_filt(GetExceptionInformation())) {}
    printf("  host %s: returned 0x%llx; fault rip=%p (fn %p) rsp=%p, filter rsp=%p (%lld bytes below)\n", what, r, (void*)g_hctx.Rip,
           (void*)fn, (void*)g_hctx.Rsp, (void*)g_hfilter_sp, (long long)(g_hctx.Rsp - g_hfilter_sp));
}

static PVOID g_enclave;
static SIZE_T g_size = 0x10000000;   // PROBE_SIZE environment variable overrides
static PROBE_IO g_io;

static void fail(const char* what)
{
    DWORD e = GetLastError();
    printf("FAIL %s: GetLastError=%lu (0x%08lx)\n", what, e, e);
}

static bool create(DWORD threads, bool debug = true, const wchar_t* image = L"vbsenclave.dll")
{
    ENCLAVE_CREATE_INFO_VBS ci = { debug ? ENCLAVE_VBS_FLAG_DEBUG : 0u, { 0x10, 0x20, 0x30, 0x40, 0x41, 0x31, 0x21, 0x11 } };
    DWORD err = 0;
    g_enclave = CreateEnclave(GetCurrentProcess(), nullptr, g_size, 0, ENCLAVE_TYPE_VBS, &ci, sizeof(ci), &err);
    if (!g_enclave) { fail("CreateEnclave"); printf("  EnclaveError=0x%08lx\n", err); return false; }
    if (!LoadEnclaveImageW(g_enclave, image)) { fail("LoadEnclaveImageW"); return false; }
    ENCLAVE_INIT_INFO_VBS ii = {};
    ii.Length = sizeof(ii);
    ii.ThreadCount = threads;
    if (!InitializeEnclave(GetCurrentProcess(), g_enclave, &ii, ii.Length, &err)) {
        fail("InitializeEnclave"); printf("  EnclaveError=0x%08lx requested ThreadCount=%lu\n", err, threads); return false;
    }
    printf("enclave base=%p size=0x%zx debug=%d ThreadCount requested=%lu returned=%lu\n", g_enclave, g_size, debug, threads, ii.ThreadCount);
    return true;
}

static PENCLAVE_ROUTINE fn(const char* name)
{
    PENCLAVE_ROUTINE r = (PENCLAVE_ROUTINE)GetProcAddress((HMODULE)g_enclave, name);
    if (!r) fail(name);
    return r;
}

static bool call(const char* name, PROBE_IO* io)
{
    void* out = nullptr;
    PENCLAVE_ROUTINE r = fn(name);
    if (!r) return false;
    if (!CallEnclave(r, io, TRUE, &out)) { fail(name); return false; }
    fwrite(io->text, 1, io->len, stdout);
    fflush(stdout);
    return true;
}

static void finish()
{
    if (!TerminateEnclave(g_enclave, TRUE)) fail("TerminateEnclave");
    if (!DeleteEnclave(g_enclave)) fail("DeleteEnclave");
}

extern "C" void* WINAPI HostEcho(void* p) { return (char*)p + 1; }

static double now_ns()
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}

struct THREAD_ARG { PENCLAVE_ROUTINE r; PROBE_IO io; BOOL ok; DWORD gle; DWORD tid; };

static BOOL g_wait_for_thread = TRUE;

static DWORD WINAPI host_thread(void* p)
{
    THREAD_ARG* a = (THREAD_ARG*)p;
    void* out = nullptr;
    a->tid = GetCurrentThreadId();
    a->ok = CallEnclave(a->r, &a->io, g_wait_for_thread, &out);
    a->gle = a->ok ? 0 : GetLastError();
    return 0;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    printf("\nHOST: unhandled exception 0x%08lx at %p; live log so far:\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    fwrite(g_io.text, 1, g_io.len, stdout);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(void* p)
{
    DWORD ms = (DWORD)(ULONG_PTR)p;
    Sleep(ms);
    printf("\nWATCHDOG: test still running after %lu ms; live log so far:\n", ms);
    fwrite(g_io.text, 1, g_io.len, stdout);
    printf("\nWATCHDOG: terminating the process\n");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 99);
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(crash_filter);
    if (getenv("PROBE_SIZE")) g_size = (SIZE_T)strtoull(getenv("PROBE_SIZE"), nullptr, 0);
    {
        const char* w = getenv("PROBE_WATCHDOG_MS");
        CloseHandle(CreateThread(nullptr, 0, watchdog, (void*)(ULONG_PTR)(w ? atoi(w) : 60000), 0, nullptr));
    }
    const char* t = argc > 1 ? argv[1] : "basic";
    printf("== host test '%s' pid=%lu\n", t, GetCurrentProcessId());

    if (!strcmp(t, "basic")) {
        if (!create(1)) return 3;
        g_io.args[0] = (ULONG_PTR)g_enclave; g_io.args[1] = g_size;
        g_io.args[2] = argc > 2 ? strtoull(argv[2], nullptr, 0) : 0;   // phases bitmask, 0 = all
        call("E_Mem", &g_io);
        finish();
    } else if (!strcmp(t, "exec")) {           // exec <group>
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = argc > 2 ? atoi(argv[2]) : 1;
        bool ok = call("E_Exec", &g_io);
        printf("E_Exec returned to the host: %d\n", ok);
        if (!ok && g_io.len) { printf("partial log:\n"); fwrite(g_io.text, 1, g_io.len, stdout); }
        finish();
    } else if (!strcmp(t, "fsbase")) {        // FS base kept across exit/entry and host threads
        if (!create(1)) return 3;
        unsigned long long h0 = 0;
        __try { h0 = _readfsbase_u64(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("host rdfsbase: exception 0x%08lx\n", GetExceptionCode()); }
        printf("host FS base before = 0x%llx, host TEB=%p\n", h0, (void*)NtCurrentTeb());
        g_io.args[0] = 4; g_io.args[1] = 0x00007abcdef01230ull;
        call("E_Exec", &g_io);
        unsigned long long h1 = 0;
        __try { h1 = _readfsbase_u64(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        printf("host FS base after first call = 0x%llx\n", h1);
        g_io.args[1] = 0;
        call("E_Exec", &g_io);
        printf("second entry saw 0x%llx (0x7abcdef01230 = kept across exit/entry on the same host thread)\n", g_io.out[0]);
        THREAD_ARG a = {};
        a.r = fn("E_Exec"); a.io.args[0] = 4; a.io.args[1] = 0;
        HANDLE h = CreateThread(nullptr, 0, host_thread, &a, 0, nullptr);
        WaitForSingleObject(h, INFINITE);
        printf("entry from another host thread saw FS base 0x%llx (ok=%d)\n", a.io.out[0], a.ok);
        finish();
    } else if (!strcmp(t, "fsocall")) {       // fsocall [n]: FS/GS base, ArbitraryUserPointer, registers across OCALLs
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = 6; g_io.args[1] = (ULONG_PTR)HostEcho; g_io.args[2] = argc > 2 ? strtoull(argv[2], nullptr, 0) : 100000;
        call("E_Exec", &g_io);
        finish();
    } else if (!strcmp(t, "fsspin")) {        // fsspin [iters]: FS base and registers under preemption (run several at once)
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = 7; g_io.args[1] = argc > 2 ? strtoull(argv[2], nullptr, 0) : 0;
        call("E_Exec", &g_io);
        finish();
    } else if (!strcmp(t, "redzone")) {       // redzone [iters]
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = 5; g_io.args[1] = argc > 2 ? strtoull(argv[2], nullptr, 0) : 0;
        call("E_Exec", &g_io);
        finish();
    } else if (!strcmp(t, "offstack")) {      // offstack <1|2|3>
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = 8; g_io.args[1] = argc > 2 ? atoi(argv[2]) : 1;
        bool ok = call("E_Exec", &g_io);
        printf("E_Exec returned to the host: %d\n", ok);
        finish();
    } else if (!strcmp(t, "stack") || !strcmp(t, "cfg") || !strcmp(t, "teb")) {
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = !strcmp(t, "stack") ? 9 : !strcmp(t, "cfg") ? 10 : 11;
        call("E_Exec", &g_io);
        finish();
    } else if (!strcmp(t, "exccost")) {      // exccost [n]
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = 13; g_io.args[1] = argc > 2 ? strtoull(argv[2], nullptr, 0) : 10000;
        for (int rep = 0; rep < 3; rep++) call("E_Exec", &g_io);
        finish();
    } else if (!strcmp(t, "rpm")) {          // rpm [nodebug]: can the host read enclave memory with ReadProcessMemory?
        bool dbg = !(argc > 2 && !strcmp(argv[2], "nodebug"));
        if (!create(1, dbg)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = 9;
        call("E_Exec", &g_io);   // leaves "StackBase=" in the enclave's log buffer (enclave .data)
        static unsigned char page[4096];
        size_t readable = 0, total = 0, hits = 0, nonzero = 0;
        for (SIZE_T off = 0; off < g_size; off += 4096) {
            SIZE_T n = 0;
            total++;
            if (!ReadProcessMemory(GetCurrentProcess(), (char*)g_enclave + off, page, 4096, &n) || n != 4096) continue;
            readable++;
            bool nz = false;
            for (int i = 0; i < 4096; i++) if (page[i]) { nz = true; break; }
            if (nz) nonzero++;
            for (int i = 0; i + 10 <= 4096; i++) if (!memcmp(page + i, "StackBase=", 10)) { hits++; if (hits == 1) printf("found the enclave's log text at enclave+0x%zx\n", (size_t)off + i); }
        }
        printf("ReadProcessMemory over the enclave (debug=%d): %zu of %zu pages readable, %zu of them non-zero, %zu hits of the enclave's log text\n", dbg, readable, total, nonzero, hits);
        finish();
    } else if (!strcmp(t, "stackgrow")) {    // stackgrow [KiB]
        if (!create(1)) return 3;
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = 12; g_io.args[1] = argc > 2 ? strtoull(argv[2], nullptr, 0) : 256;
        bool ok = call("E_Exec", &g_io);
        printf("E_Exec returned to the host: %d\n", ok);
        finish();
    } else if (!strcmp(t, "hostcpu")) {       // host-only survey (no enclave): the same probes in a normal process
        static const unsigned leaves[][2] = {
            { 0, 0 }, { 1, 0 }, { 6, 0 }, { 7, 0 }, { 7, 1 }, { 0xb, 0 }, { 0xd, 0 }, { 0xd, 1 }, { 0x14, 0 }, { 0x19, 0 },
            { 0x40000000, 0 }, { 0x40000001, 0 }, { 0x40000003, 0 }, { 0x80000000, 0 }, { 0x80000001, 0 }, { 0x80000007, 0 },
            { 0x80000008, 0 },
        };
        for (auto& l : leaves) {
            int r[4];
            __cpuidex(r, (int)l[0], (int)l[1]);
            printf("  cpuid(0x%08x,%u): eax=%08x ebx=%08x ecx=%08x edx=%08x\n", l[0], l[1], r[0], r[1], r[2], r[3]);
        }
        printf("host XCR0=0x%llx\n", (unsigned long long)_xgetbv(0));
        __try { printf("host XINUSE (xgetbv 1)=0x%llx\n", (unsigned long long)_xgetbv(1)); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("host xgetbv(1): exception 0x%08lx\n", GetExceptionCode()); }
        printf("host GetEnabledXStateFeatures=0x%llx\n", (unsigned long long)GetEnabledXStateFeatures());
        for (int pf = 0; pf < 96; pf++) if (IsProcessorFeaturePresent(pf)) printf("PF%d ", pf);
        printf("\n(IsProcessorFeaturePresent in the host)\n");
        bool fsgs = true;
        __try { printf("host rdfsbase=0x%llx rdgsbase=0x%llx TEB=%p\n", _readfsbase_u64(), _readgsbase_u64(), (void*)NtCurrentTeb()); }
        __except (EXCEPTION_EXECUTE_HANDLER) { printf("host rdfsbase/rdgsbase: exception 0x%08lx (CR4.FSGSBASE off for user mode)\n", GetExceptionCode()); fsgs = false; }
        if (fsgs) {
            unsigned long long iters = argc > 2 ? strtoull(argv[2], nullptr, 0) : 2000000000ull;
            __try {
                unsigned long long v = 0x00007ee300000010ull, changes = 0, first = 0;
                _writefsbase_u64(v);
                double t0 = now_ns();
                for (unsigned long long i = 0; i < iters; i++) {
                    unsigned long long g = _readfsbase_u64();
                    if (g != v) { if (!changes) first = g; changes++; _writefsbase_u64(v); }
                }
                double t1 = now_ns();
                Sleep(100);
                unsigned long long after_sleep = _readfsbase_u64();
                _writefsbase_u64(0);
                printf("host wrfsbase: %llu iterations (%.0f ms): %llu changes (first 0x%llx); after Sleep(100): 0x%llx\n", iters, (t1 - t0) / 1e6, changes, first, after_sleep);
            } __except (EXCEPTION_EXECUTE_HANDLER) { printf("host wrfsbase: exception 0x%08lx\n", GetExceptionCode()); }
        }
    } else if (!strcmp(t, "hostexc")) {       // host-only proxies of the red zone and resume tests (no enclave)
        host_trap("int3, resume rip+1", probe_int3, 1);
        host_trap("ud2, resume rip+2", probe_ud2, 2);
        host_trap("div0, resume rip+2", probe_div0, 2);
        host_trap("red zone pattern + ud2 (returns changed qwords of 16)", probe_redzone_ud2, 2);
        unsigned long long iters = argc > 2 ? strtoull(argv[2], nullptr, 0) : 2000000000ull;
        double t0 = now_ns();
        long long rz = probe_redzone_spin(iters);
        printf("  host red zone through a %llu-iteration spin (%.0f ms): changed mask 0x%llx\n", iters, (now_ns() - t0) / 1e6, rz);
        t0 = now_ns();
        long long m = probe_regs_spin(iters / 2);
        printf("  host registers through a %llu-iteration spin (%.0f ms): changed mask 0x%llx\n", iters / 2, (now_ns() - t0) / 1e6, m);
        char* st = (char*)VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        void* sb = tib->StackBase; void* sl = tib->StackLimit;
        tib->StackLimit = st; tib->StackBase = st + 65536;
        long long r = probe_fault_offstack(st + 65536 - 64);
        tib->StackLimit = sl; tib->StackBase = sb;
        printf("  host ud2 with rsp off the thread stack (TEB bounds widened, handler in unwind data): returned %lld, handler calls %ld\n", r, g_lh_calls);
    } else if (!strcmp(t, "cost")) {           // cost <n>
        int n = argc > 2 ? atoi(argv[2]) : 100000;
        if (!create(1)) return 3;
        PENCLAVE_ROUTINE r = fn("E_Nop");
        void* out;
        for (int i = 0; i < 1000; i++) CallEnclave(r, (void*)(ULONG_PTR)i, TRUE, &out);   // warm up
        for (int rep = 0; rep < 3; rep++) {
            int bad = 0;
            double t0 = now_ns();
            for (int i = 0; i < n; i++) if (!CallEnclave(r, (void*)(ULONG_PTR)i, TRUE, &out) || out != (void*)(ULONG_PTR)i) bad++;
            double t1 = now_ns();
            printf("host->enclave->host (E_Nop): %d calls, %d wrong, %.0f ns per call\n", n, bad, (t1 - t0) / n);
        }
        // a plain host function for comparison
        volatile void* (WINAPI * volatile he)(void*) = (volatile void* (WINAPI*)(void*))HostEcho;
        double t0 = now_ns();
        for (int i = 0; i < n; i++) he((void*)(ULONG_PTR)i);
        printf("host function call for comparison: %.1f ns per call\n", (now_ns() - t0) / n);
        memset(&g_io, 0, sizeof(g_io));
        g_io.args[0] = (ULONG_PTR)HostEcho; g_io.args[1] = n;
        for (int rep = 0; rep < 3; rep++) call("E_CallOut", &g_io);
        finish();
    } else if (!strcmp(t, "threads")) {        // threads <ThreadCount> <host threads>
        DWORD tc = argc > 2 ? atoi(argv[2]) : 2;
        int m = argc > 3 ? atoi(argv[3]) : 3;
        if (argc > 4) g_wait_for_thread = atoi(argv[4]);   // 0: CallEnclave(fWaitForThread=FALSE)
        if (!create(tc)) return 3;
        THREAD_ARG* a = (THREAD_ARG*)calloc(m + 1, sizeof(THREAD_ARG));
        HANDLE* h = (HANDLE*)calloc(m + 1, sizeof(HANDLE));
        for (int i = 0; i < m; i++) {
            a[i].r = fn("E_Thread"); a[i].io.args[0] = 1; a[i].io.args[1] = 4000;
            h[i] = CreateThread(nullptr, 0, host_thread, &a[i], 0, nullptr);
        }
        Sleep(1000);
        double t0 = now_ns();
        a[m].r = fn("E_Thread"); a[m].io.args[0] = 2;
        h[m] = CreateThread(nullptr, 0, host_thread, &a[m], 0, nullptr);
        WaitForMultipleObjects(m + 1, h, TRUE, INFINITE);
        printf("waker done after %.1f ms (ok=%d gle=%lu)\n", (now_ns() - t0) / 1e6, a[m].ok, a[m].gle);
        for (int i = 0; i < m; i++)
            printf(" waiter %d host tid %lu: CallEnclave ok=%d gle=%lu; in enclave: WaitOnAddress ok=%llu gle=%llu waited=%llu us enclave tid=%llu\n",
                   i, a[i].tid, a[i].ok, a[i].gle, a[i].io.out[0], a[i].io.out[1], a[i].io.out[2], a[i].io.out[3]);
        finish();
    } else if (!strcmp(t, "spin")) {           // spin <threads> <ms>
        int m = argc > 2 ? atoi(argv[2]) : 16;
        int ms = argc > 3 ? atoi(argv[3]) : 2000;
        if (!create(m)) return 3;
        THREAD_ARG* a = (THREAD_ARG*)calloc(m, sizeof(THREAD_ARG));
        HANDLE* h = (HANDLE*)calloc(m, sizeof(HANDLE));
        for (int i = 0; i < m; i++) {
            a[i].r = fn("E_Thread"); a[i].io.args[0] = 3; a[i].io.args[1] = ms;
            h[i] = CreateThread(nullptr, 0, host_thread, &a[i], 0, nullptr);
        }
        WaitForMultipleObjects(m, h, TRUE, INFINITE);
        for (int i = 0; i < m; i++)
            printf(" spinner %d: ok=%d gle=%lu max QPC gap %llu us, %llu QPC reads\n", i, a[i].ok, a[i].gle, a[i].io.out[0], a[i].io.out[1]);
        finish();
    } else if (!strcmp(t, "tls")) {
        if (!create(1)) return 3;
        THREAD_ARG a = {};
        a.r = fn("E_Thread"); a.io.args[0] = 4;
        host_thread(&a);
        printf("TlsAlloc=%llu TlsSetValue/GLE=%llu TlsGetValue=0x%llx ok=%d\n", a.io.out[0], a.io.out[1], a.io.out[2], a.ok);
        finish();
    } else if (!strcmp(t, "api")) {            // api [sealed-blob-file]
        if (!create(1)) return 3;
        static unsigned char blob[4096];
        if (argc > 2) {
            FILE* f = fopen(argv[2], "rb");
            if (f) { g_io.args[1] = fread(blob, 1, sizeof(blob), f); g_io.args[0] = (ULONG_PTR)blob; fclose(f); }
        }
        call("E_Api", &g_io);
        if (g_io.out[0]) { FILE* f = fopen("report.bin", "wb"); fwrite(g_io.text + sizeof(g_io.text) / 2, 1, (size_t)g_io.out[0], f); fclose(f); printf("report saved to report.bin\n"); }
        if (g_io.out[1]) { FILE* f = fopen("sealed.bin", "wb"); fwrite(g_io.text + sizeof(g_io.text) / 2 + 8192, 1, (size_t)g_io.out[1], f); fclose(f); printf("sealed blob (SAME_IMAGE) saved to sealed.bin\n"); }
        finish();
    } else if (!strcmp(t, "hostread")) {
        if (!create(1)) return 3;
        unsigned char* p = (unsigned char*)g_enclave;
        __try { printf("host read of enclave base: 0x%02x\n", p[0]); }
        __except (EXCEPTION_EXECUTE_HANDLER) { printf("host read of enclave base: exception 0x%08lx\n", GetExceptionCode()); }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)))
            printf("host VirtualQuery(enclave base): size=0x%zx state=0x%lx protect=0x%lx type=0x%lx\n", mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type);
        unsigned char b; SIZE_T n = 0;
        BOOL ok = ReadProcessMemory(GetCurrentProcess(), p + 0x1000, &b, 1, &n);
        printf("host ReadProcessMemory(enclave image+0x1000) ok=%d gle=%lu\n", ok, ok ? 0 : GetLastError());
        finish();
    } else if (!strcmp(t, "loaddata")) {
        ENCLAVE_CREATE_INFO_VBS ci = { ENCLAVE_VBS_FLAG_DEBUG, {0} };
        DWORD err = 0;
        g_enclave = CreateEnclave(GetCurrentProcess(), nullptr, g_size, 0, ENCLAVE_TYPE_VBS, &ci, sizeof(ci), &err);
        if (!g_enclave) { fail("CreateEnclave"); return 3; }
        if (!LoadEnclaveImageW(g_enclave, L"vbsenclave.dll")) { fail("LoadEnclaveImageW"); return 4; }
        static __declspec(align(4096)) unsigned char page[4096] = { 0xB8, 42, 0, 0, 0, 0xC3 };
        DWORD prots[] = { PAGE_EXECUTE_READ, PAGE_READWRITE };
        static unsigned char info[256];
        DWORD lens[] = { 0, 4, 8, 16, 64, 256 };
        for (int i = 0; i < 2; i++) for (int l = 0; l < 6; l++) {
            SIZE_T written = 0; err = 0;
            void* where = (char*)g_enclave + 0x8000000 + i * 0x10000;
            BOOL ok = LoadEnclaveData(GetCurrentProcess(), where, page, 4096, prots[i], lens[l] ? info : nullptr, lens[l], &written, &err);
            printf("LoadEnclaveData(before init, %p, prot 0x%lx, info len %lu) ok=%d gle=%lu EnclaveError=0x%lx written=%zu\n", where, prots[i], lens[l], ok, ok ? 0 : GetLastError(), err, written);
        }
        void* va = VirtualAlloc((char*)g_enclave + 0x9000000, 4096, MEM_COMMIT, PAGE_EXECUTE_READ);
        printf("host VirtualAlloc(MEM_COMMIT, RX) inside enclave range before init -> %p gle=%lu\n", va, va ? 0 : GetLastError());
        ENCLAVE_INIT_INFO_VBS ii = { sizeof(ii), 1 };
        BOOL ok = InitializeEnclave(GetCurrentProcess(), g_enclave, &ii, ii.Length, &err);
        printf("InitializeEnclave ok=%d gle=%lu\n", ok, ok ? 0 : GetLastError());
        finish();
    } else if (!strcmp(t, "rwx")) {            // rwx <flags>: the variant image with an RWX section
        if (!create(1, true, L"vbsenclave-rwx.dll")) return 3;
        g_io.args[0] = argc > 2 ? atoi(argv[2]) : 0;
        call("E_Rwx", &g_io);
        finish();
    } else if (!strcmp(t, "nodebug")) {
        // the image is debuggable; create without the debug flag
        if (!create(1, false)) return 3;
        printf("non-debug creation of a debuggable image: ok\n");
        finish();
    } else {
        printf("unknown test\n");
        return 1;
    }
    printf("== done '%s'\n", t);
    return 0;
}
