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
static void* CALLBACK pal_vbs_ocall(void* param) {
    pal_vbs_ocall_args_t* a = (pal_vbs_ocall_args_t*)param;
    switch (a->ocall_no) {
        case PAL_VBS_OCALL_CONSOLE_WRITE: {
            uint64_t len = a->a0;
            if (len > PAL_VBS_CONBUF_SIZE)
                len = PAL_VBS_CONBUF_SIZE;
            fwrite(g_shared.conbuf, 1, (size_t)len, stdout);
            fflush(stdout);
            a->retval = len;
            break;
        }
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
                                                            : PAL_VBS_MODE_SELFTEST;

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

    TerminateEnclave(enclave, TRUE);
    DeleteEnclave(enclave);
    return (int)g_shared.result;
}
