/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * cntvct -- EL0 reads of the AArch64 virtual counter (CNTVCT_EL0) and its frequency
 * (CNTFRQ_EL0), as OpenSSL and other libraries do. Both reads trap (EC 0x18) when the kernel
 * leaves CNTKCTL_EL1.EL0VCTEN clear. Checks that the counter is non-zero, that it advances,
 * and that a 10-ms nanosleep moves it by at least 10 ms of ticks; then reads it once on each
 * CPU (sched_setaffinity), since every CPU sets the bit for itself.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

static uint64_t cntvct(void) {
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

int main(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t a = cntvct();
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
    nanosleep(&ts, NULL);
    uint64_t b = cntvct();
    uint64_t us = freq ? (b - a) * 1000000 / freq : 0;
    int ok = a != 0 && b > a && freq != 0 && us >= 9000 && us < 1000000;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int pinned = 0;
    for (long cpu = 0; cpu < cpus; cpu++) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) < 0)
            continue;
        sched_yield();
        uint64_t c = cntvct();
        if (c <= b)
            ok = 0;
        b = c;
        pinned++;
    }
    printf("cntvct: %s (freq %lu Hz, first %lu, 10-ms sleep = %lu us, read on %d of %ld CPUs)\n",
           ok ? "ok" : "FAIL", (unsigned long)freq, (unsigned long)a, (unsigned long)us, pinned,
           cpus);
    return ok ? 0 : 1;
}
