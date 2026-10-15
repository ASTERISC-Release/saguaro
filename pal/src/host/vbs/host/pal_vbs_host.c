/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Host runtime of the Windows VBS-enclave PAL (phase 1 skeleton): a Windows ARM64 executable that
 * creates the enclave, loads the signed image, initialises it with N threads, enters it with the
 * E_PalMain ECALL, and serves the enclave's OCALLs (console, host time, exit). It is the untrusted
 * counterpart to the enclave DLL and follows Microsoft's VbsEnclave sample (MIT) for the create/
 * load/initialize sequence.
 *
 * Usage: pal_vbs_host.exe [image.dll] [thread_count] [selftest|boot]
 *   selftest (default): the PAL-level self-test; boot: the packed PAL, LibOS and application.
 */

#include <windows.h>
#include <enclaveapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal_vbs_abi.h"

static pal_vbs_shared_t g_shared; /* host memory; the enclave reads/writes it via the copy helpers */
static PENCLAVE_ROUTINE g_thread_start;  /* E_ThreadStart */
static CRITICAL_SECTION g_console_lock;  /* console lines of several enclave threads */
static volatile LONG g_threads_started, g_threads_failed;

static void fail(const char* what) {
    DWORD e = GetLastError();
    fprintf(stderr, "FAIL %s: GetLastError=%lu (0x%08lx)\n", what, e, e);
}

/* Unix time in nanoseconds from the Windows wall clock. */
static uint64_t host_time_ns(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    const uint64_t EPOCH_DIFF_100NS = 116444736000000000ull;            /* 1601 -> 1970 */
    if (t < EPOCH_DIFF_100NS)
        return 0;
    return (t - EPOCH_DIFF_100NS) * 100ull;
}

/*
 * The single host OCALL dispatcher. Its address is handed to the enclave in g_shared.ocall; the
 * enclave calls it from inside the enclave with CallEnclave(ocall, &g_shared.ocall_args, ...).
 * The signature matches LPENCLAVE_ROUTINE (void* CALLBACK fn(void*)).
 */
static void* CALLBACK pal_vbs_ocall(void* param);

/* A host thread for the enclave: enters E_ThreadStart with its own block; returns when the enclave
 * thread exits. CallEnclave waits for a free enclave thread slot (ThreadCount). */
static DWORD WINAPI host_thread_proc(void* param) {
    pal_vbs_thread_block_t* blk = param;
    void* out = NULL;
    if (!CallEnclave(g_thread_start, blk, TRUE, &out))
        fprintf(stderr, "[host] CallEnclave(E_ThreadStart) failed: %lu\n", GetLastError());
    else if (out)
        fprintf(stderr, "[host] E_ThreadStart refused the thread: %lld\n", (long long)(intptr_t)out);
    free(blk);
    return 0;
}

static void* CALLBACK pal_vbs_ocall(void* param) {
    pal_vbs_ocall_args_t* a = (pal_vbs_ocall_args_t*)param;
    switch (a->ocall_no) {
        case PAL_VBS_OCALL_CONSOLE_WRITE: {
            /* a1: the calling thread's console buffer (g_shared.conbuf or its thread block) */
            uint64_t len = a->a0;
            if (len > PAL_VBS_CONBUF_SIZE)
                len = PAL_VBS_CONBUF_SIZE;
            const uint8_t* buf = a->a1 ? (const uint8_t*)(uintptr_t)a->a1 : g_shared.conbuf;
            EnterCriticalSection(&g_console_lock);
            fwrite(buf, 1, (size_t)len, stdout);
            fflush(stdout);
            LeaveCriticalSection(&g_console_lock);
            a->retval = len;
            break;
        }
        case PAL_VBS_OCALL_THREAD_CREATE: {
            pal_vbs_thread_block_t* blk = calloc(1, sizeof(*blk));
            HANDLE th = NULL;
            if (blk) {
                blk->token = a->a0;
                blk->ocall = (void*)pal_vbs_ocall;
                th = CreateThread(NULL, 0, host_thread_proc, blk, 0, NULL);
            }
            if (th) {
                CloseHandle(th);
                InterlockedIncrement(&g_threads_started);
                a->retval = 0;
            } else {
                free(blk);
                InterlockedIncrement(&g_threads_failed);
                a->retval = (uint64_t)-1;
            }
            break;
        }
        case PAL_VBS_OCALL_EXIT_PROCESS:
            fflush(stdout);
            fprintf(stderr, "[host] enclave thread ended the process, code=%lld (threads started "
                    "%ld)\n", (long long)a->a0, g_threads_started);
            /* TerminateProcess, not ExitProcess: other threads are still inside the enclave, and an
             * orderly process exit faults on them (exit code 0xC0000005 instead of this one) */
            TerminateProcess(GetCurrentProcess(), (UINT)a->a0);
            break;
        case PAL_VBS_OCALL_GET_TIME_NS:
            a->retval = host_time_ns();
            break;
        case PAL_VBS_OCALL_EXIT:
            /* record only; the host tears the enclave down after E_PalMain returns */
            a->retval = 0;
            fprintf(stderr, "[host] enclave requested exit, code=%lld\n", (long long)a->a0);
            break;
        default:
            fprintf(stderr, "[host] unknown OCALL %llu\n", (unsigned long long)a->ocall_no);
            a->retval = (uint64_t)-1;
            break;
    }
    return NULL;
}

/* Reports exceptions the host process sees (including ones raised for an enclave thread) before
 * the process dies: code, address, thread, and whether the address lies in the enclave. */
static uint64_t g_enclave_lo, g_enclave_hi;
static LONG CALLBACK host_exception_report(EXCEPTION_POINTERS* ep) {
    uint64_t pc = (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "[host] exception 0x%08lx at %p (%s) thread %lu, pc %p sp %p lr %p",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
            (pc >= g_enclave_lo && pc < g_enclave_hi) ? "in the enclave" : "host",
            GetCurrentThreadId(), (void*)(uintptr_t)ep->ContextRecord->Pc,
            (void*)(uintptr_t)ep->ContextRecord->Sp, (void*)(uintptr_t)ep->ContextRecord->Lr);
    if (ep->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, " access %llu addr %p", (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
                (void*)(uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const wchar_t* image = L"pal_vbs_enclave.dll";
    wchar_t imagebuf[MAX_PATH];
    if (argc > 1) {
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, imagebuf, MAX_PATH);
        image = imagebuf;
    }
    DWORD threads = argc > 2 ? (DWORD)atoi(argv[2]) : 1;

    ENCLAVE_CREATE_INFO_VBS ci = { ENCLAVE_VBS_FLAG_DEBUG,
                                   { 0x10, 0x20, 0x30, 0x40, 0x41, 0x31, 0x21, 0x11 } };
    DWORD err = 0;
    SIZE_T size = 0x10000000; /* must match the image's configured size */
    PVOID enclave = CreateEnclave(GetCurrentProcess(), NULL, size, 0, ENCLAVE_TYPE_VBS, &ci,
                                  sizeof(ci), &err);
    if (!enclave) { fail("CreateEnclave"); fprintf(stderr, "  EnclaveError=0x%08lx\n", err); return 3; }
    if (!LoadEnclaveImageW(enclave, image)) { fail("LoadEnclaveImageW"); return 4; }

    ENCLAVE_INIT_INFO_VBS ii;
    memset(&ii, 0, sizeof(ii));
    ii.Length = sizeof(ii);
    ii.ThreadCount = threads;
    if (!InitializeEnclave(GetCurrentProcess(), enclave, &ii, ii.Length, &err)) {
        fail("InitializeEnclave");
        fprintf(stderr, "  EnclaveError=0x%08lx requested ThreadCount=%lu\n", err, threads);
        return 5;
    }
    g_enclave_lo = (uint64_t)(uintptr_t)enclave;
    g_enclave_hi = g_enclave_lo + size;
    AddVectoredExceptionHandler(1, host_exception_report);
    printf("[host] enclave base=%p size=0x%zx ThreadCount requested=%lu returned=%lu\n",
           enclave, size, threads, ii.ThreadCount);

    memset(&g_shared, 0, sizeof(g_shared));
    g_shared.abi_version  = PAL_VBS_ABI_VERSION;
    g_shared.thread_count = ii.ThreadCount;
    g_shared.enclave_base = (uint64_t)(uintptr_t)enclave;
    g_shared.enclave_size = size;
    g_shared.ocall        = (void*)pal_vbs_ocall;
    g_shared.result       = -1;
    g_shared.mode = (argc > 3 && !strcmp(argv[3], "boot")) ? PAL_VBS_MODE_BOOT
                    : (argc > 3 && !strcmp(argv[3], "tpidr")) ? PAL_VBS_MODE_TPIDR
                    : (argc > 3 && !strcmp(argv[3], "tebslot")) ? PAL_VBS_MODE_TEBSLOT
                    : PAL_VBS_MODE_SELFTEST;

    InitializeCriticalSection(&g_console_lock);
    g_thread_start = (PENCLAVE_ROUTINE)GetProcAddress((HMODULE)enclave, "E_ThreadStart");
    PENCLAVE_ROUTINE pal_main = (PENCLAVE_ROUTINE)GetProcAddress((HMODULE)enclave, "E_PalMain");
    if (!pal_main) { fail("GetProcAddress(E_PalMain)"); return 6; }

    void* out = NULL;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    BOOL ok = CallEnclave(pal_main, &g_shared, TRUE, &out);
    QueryPerformanceCounter(&t1);
    if (!ok) { fail("CallEnclave(E_PalMain)"); }

    double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart;
    if (g_shared.mode == PAL_VBS_MODE_BOOT)
        printf("[host] E_PalMain (boot) returned ok=%d result=%lld in %.1f us\n", ok,
               (long long)g_shared.result, us);
    else
        printf("[host] E_PalMain returned ok=%d ret=%p result=%lld (%llu/%llu self-tests passed) "
               "in %.1f us\n", ok, out, (long long)g_shared.result,
               (unsigned long long)g_shared.selftests_passed,
               (unsigned long long)g_shared.selftests_run, us);

    if (g_shared.mode == PAL_VBS_MODE_BOOT && g_threads_started) {
        /* other enclave threads may still be inside (blocked): end the process without waiting */
        fprintf(stderr, "[host] enclave threads started: %ld, failed to start: %ld\n",
                g_threads_started, g_threads_failed);
        fflush(stdout);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), (UINT)g_shared.result);
    }
    TerminateEnclave(enclave, TRUE);
    DeleteEnclave(enclave);
    return (int)g_shared.result;
}
