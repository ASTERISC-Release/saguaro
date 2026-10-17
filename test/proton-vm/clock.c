// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The ASTERISC Research Lab
// Windows clock check for the Proton study: QueryPerformanceCounter/Frequency, GetTickCount64,
// timeGetTime and GetSystemTimePreciseAsFileTime across Sleep() and across a busy loop, so that a
// game spinning on the performance counter can be told apart from a clock that does not advance.
// Build: x86_64-w64-mingw32-gcc -O2 -o clock.exe clock.c -lwinmm
#include <stdio.h>
#include <windows.h>

static double qpc_ms(LARGE_INTEGER f) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double ft_ms(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u = { .LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime };
    return (double)u.QuadPart / 10000.0;
}

int main(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    printf("clock: QueryPerformanceFrequency %lld Hz\n", (long long)f.QuadPart);
    for (int i = 0; i < 3; i++) {
        double q0 = qpc_ms(f), t0 = (double)GetTickCount64(), m0 = (double)timeGetTime(), s0 = ft_ms();
        Sleep(200);
        printf("clock: Sleep(200): qpc %+.1f ms, GetTickCount64 %+.0f ms, timeGetTime %+.0f ms, systime %+.1f ms\n",
               qpc_ms(f) - q0, (double)GetTickCount64() - t0, (double)timeGetTime() - m0, ft_ms() - s0);
    }
    // a busy loop of 300 ms by the performance counter, checked against the other clocks
    double q0 = qpc_ms(f), t0 = (double)GetTickCount64(), s0 = ft_ms();
    unsigned long long n = 0;
    while (qpc_ms(f) - q0 < 300.0)
        n++;
    printf("clock: busy 300 ms by qpc: %llu calls, GetTickCount64 %+.0f ms, systime %+.1f ms\n", n,
           (double)GetTickCount64() - t0, ft_ms() - s0);
    printf("clock: done\n");
    return 0;
}
