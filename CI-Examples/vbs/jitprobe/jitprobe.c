/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Research probe (PLAN-rwx.md): the memory requests of a JIT or of the JVM's code cache, one by one,
 * with what the enclave answers. Exit code 0 when every request that a JIT needs was refused as
 * expected is NOT a goal: the program only reports. */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static void report(const char* what, void* r) {
    if (r == MAP_FAILED)
        printf("%-52s -> failed: %s\n", what, strerror(errno));
    else
        printf("%-52s -> ok\n", what);
}

static void rep_int(const char* what, int r) {
    if (r < 0)
        printf("%-52s -> failed: %s\n", what, strerror(errno));
    else
        printf("%-52s -> ok\n", what);
}

int main(void) {
    size_t sz = 1 << 20;
    /* the JVM code cache: reserve, commit read-write, then add execute */
    void* r = mmap(NULL, sz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    report("mmap(1 MiB, PROT_NONE) reserve", r);
    if (r != MAP_FAILED) {
        rep_int("mprotect(reserve, PROT_READ|PROT_WRITE)", mprotect(r, sz, PROT_READ | PROT_WRITE));
        rep_int("mprotect(reserve, PROT_READ|PROT_WRITE|PROT_EXEC)",
                mprotect(r, sz, PROT_READ | PROT_WRITE | PROT_EXEC));
        rep_int("mprotect(reserve, PROT_READ|PROT_EXEC)", mprotect(r, sz, PROT_READ | PROT_EXEC));
    }
    report("mmap(1 MiB, RWX) anonymous", mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    report("mmap(1 MiB, R|X) anonymous", mmap(NULL, sz, PROT_READ | PROT_EXEC,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    /* write a function, then make it executable */
    unsigned char* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    report("mmap(4 KiB, RW) for generated code", p);
    if (p != MAP_FAILED) {
#if defined(__x86_64__)
        static const unsigned char code[] = { 0xb8, 42, 0, 0, 0, 0xc3 }; /* mov eax, 42; ret */
#else
        static const unsigned char code[] = { 0x40, 0x05, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6 };
        /* mov w0, #42; ret */
#endif
        memcpy(p, code, sizeof(code));
        int ret = mprotect(p, 4096, PROT_READ | PROT_EXEC);
        rep_int("mprotect(generated code, PROT_READ|PROT_EXEC)", ret);
        if (ret == 0)
            printf("generated function returned %d\n", ((int (*)(void))p)());
    }
    /* a protection change over the program's own code (what RELRO or a hardening step does) */
    extern char __executable_start;
    uintptr_t a = (uintptr_t)&__executable_start & ~(uintptr_t)4095;
    rep_int("mprotect(own code page, PROT_READ|PROT_EXEC) (same)", mprotect((void*)a, 4096, PROT_READ | PROT_EXEC));
    rep_int("mprotect(own code page, PROT_READ|PROT_WRITE)", mprotect((void*)a, 4096, PROT_READ | PROT_WRITE));
    rep_int("mprotect(own code page, PROT_NONE)", mprotect((void*)a, 4096, PROT_NONE));
    return 0;
}
