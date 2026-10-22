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
// Saguaro phase-0 spike: host of the probe enclave. The enclave creation sequence follows
// Microsoft's VbsEnclave sample (MIT). Usage: host.exe <test> [args]; see main().

#include <windows.h>
#include <enclaveapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"

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
    } else if (!strcmp(t, "tpidr")) {
        if (!create(1)) return 3;
        unsigned long long h0 = _ReadStatusReg(ARM64_TPIDR_EL0);
        printf("host TPIDR_EL0 before = 0x%llx, host x18(TEB)=%p\n", h0, (void*)NtCurrentTeb());
        g_io.args[0] = 4; g_io.args[1] = 0xabcdef0123ull;
        call("E_Exec", &g_io);
        printf("host TPIDR_EL0 after first call = 0x%llx\n", (unsigned long long)_ReadStatusReg(ARM64_TPIDR_EL0));
        g_io.args[1] = 0;
        call("E_Exec", &g_io);
        printf("second entry saw 0x%llx (0xabcdef0123 = kept across exit/entry on the same host thread)\n", g_io.out[0]);
        // another host thread
        THREAD_ARG a = {};
        a.r = fn("E_Exec"); a.io.args[0] = 4; a.io.args[1] = 0;
        HANDLE h = CreateThread(nullptr, 0, host_thread, &a, 0, nullptr);
        WaitForSingleObject(h, INFINITE);
        printf("entry from another host thread saw TPIDR_EL0 0x%llx (ok=%d)\n", a.io.out[0], a.ok);
        finish();
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
        static __declspec(align(4096)) unsigned char page[4096] = { 0x40, 0x05, 0x80, 0xd2, 0xc0, 0x03, 0x5f, 0xd6 };
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
