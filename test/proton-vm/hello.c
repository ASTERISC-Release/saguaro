/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* A Windows console program for the Proton feasibility study (dev docs: proton-vm/):
 * prints, creates a thread, uses a critical section and an event, writes and reads a file, sleeps,
 * raises and catches a structured exception (Wine implements SEH with SIGSEGV), and exits with
 * code 42. Built with mingw-w64 for x86_64 and i686 (test/proton-vm/build-tests.sh). */

#include <stdio.h>
#include <windows.h>

static CRITICAL_SECTION g_cs;
static HANDLE g_event;
static volatile LONG g_counter;

static DWORD WINAPI worker(LPVOID arg) {
    for (int i = 0; i < 1000; i++) {
        EnterCriticalSection(&g_cs);
        g_counter++;
        LeaveCriticalSection(&g_cs);
    }
    SetEvent(g_event);
    return (DWORD)(ULONG_PTR)arg;
}

/* mingw's C compiler has no __try/__except: a vectored handler and longjmp do the same job (Wine
 * turns the SIGSEGV of the NULL write into an EXCEPTION_ACCESS_VIOLATION and calls the handler). */
#include <setjmp.h>
static jmp_buf g_jmp;
static DWORD g_exc_code;
static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    g_exc_code = ep->ExceptionRecord->ExceptionCode;
    longjmp(g_jmp, 1);
}
static int seh_test(void) {
    volatile int* p = NULL;
    void* h = AddVectoredExceptionHandler(1, veh);
    int caught = 0;
    if (setjmp(g_jmp) == 0)
        *p = 1;
    else
        caught = g_exc_code == EXCEPTION_ACCESS_VIOLATION;
    RemoveVectoredExceptionHandler(h);
    return caught;
}

int main(int argc, char** argv) {
    DWORD t0 = GetTickCount();
    printf("hello from %s (%u-bit), %d args, pid %lu\n", argv[0], (unsigned)(sizeof(void*) * 8),
           argc, (unsigned long)GetCurrentProcessId());
    fflush(stdout);

    InitializeCriticalSection(&g_cs);
    g_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD tid;
    HANDLE th = CreateThread(NULL, 0, worker, (LPVOID)7, 0, &tid);
    if (!th) {
        printf("CreateThread failed: %lu\n", GetLastError());
        return 1;
    }
    DWORD w = WaitForSingleObject(g_event, 5000);
    DWORD code = 0;
    WaitForSingleObject(th, 5000);
    GetExitCodeThread(th, &code);
    printf("thread: wait=%lu counter=%ld exit=%lu\n", w, (long)g_counter, code);

    char path[MAX_PATH];
    GetTempPathA(sizeof(path), path);
    strncat(path, "proton-vm-hello.txt", sizeof(path) - strlen(path) - 1);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n = 0;
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, "proton-vm\n", 10, &n, NULL);
        CloseHandle(f);
    }
    char buf[32] = {0};
    f = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD r = 0;
    if (f != INVALID_HANDLE_VALUE) {
        ReadFile(f, buf, sizeof(buf) - 1, &r, NULL);
        CloseHandle(f);
        DeleteFileA(path);
    }
    printf("file: wrote %lu read %lu '%s'\n", n, r, r ? "proton-vm" : "");

    printf("seh: %s\n", seh_test() ? "caught access violation" : "NOT caught");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    printf("system: %lu cpus, %llu MiB RAM, page %lu, min app addr %p, max app addr %p\n",
           si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20), si.dwPageSize,
           si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress);
    Sleep(10);
    printf("done in %lu ms\n", GetTickCount() - t0);
    return 42;
}
