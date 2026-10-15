/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Phase 3d of the VBS-enclave PAL: a dynamically linked program. The LibOS loads it and its
 * interpreter (ld.so) at their packed addresses; ld.so maps libc.so.6 itself (mmap of the file),
 * which the LibOS and the vbs PAL place at libc's packed address. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cmp(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

int main(void) {
    int v[] = { 5, 3, 9, 1, 7 };
    qsort(v, 5, sizeof(v[0]), cmp); /* libc through the PLT, a callback back into the program */
    printf("Hello from a dynamically linked program in a VBS enclave: sorted %d %d %d %d %d, "
           "strlen=%zu, pid=%d\n", v[0], v[1], v[2], v[3], v[4], strlen("enclave"), (int)getpid());
    void* h = dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD);
    printf("dlopen(libc.so.6, RTLD_NOLOAD): %s\n", h ? "found the loaded libc" : dlerror());
    /* a library loaded at run time: packed into the image too, mapped by ld.so where it lies */
    void* m = dlopen("libm.so.6", RTLD_NOW);
    double (*fn_cos)(double) = m ? (double (*)(double))dlsym(m, "cos") : NULL;
    double c = fn_cos ? fn_cos(0.0) : -1.0;
    printf("dlopen(libm.so.6) + dlsym(cos): %s, cos(0) = %g\n", m ? "ok" : dlerror(), c);
    return (v[0] == 1 && v[4] == 9 && h && c == 1.0) ? 0 : 1;
}
