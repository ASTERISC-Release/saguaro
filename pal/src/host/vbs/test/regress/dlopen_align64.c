/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* dlopen of a packed shared object whose PT_LOAD alignment is 64 KiB (libalign64.so, packed at
 * /tests/libalign64.so): glibc's ld.so maps such an object into an aligned reservation with
 * MAP_FIXED unless the packer lowered the alignment (Documentation/devel/vbs-enclave/
 * PLAN-packer-align.md). Calls into it from the main thread and from a second thread (its TLS). */

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

static int (*add)(int);

static void* thread_fn(void* arg) {
    (void)arg;
    return (void*)(long)add(2); /* this thread's copy: 40 + 2 */
}

int main(void) {
    void* h = dlopen("/tests/libalign64.so", RTLD_LAZY);
    if (!h) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    add = (int (*)(int))dlsym(h, "align64_add");
    int (*ctor_ran)(void) = (int (*)(void))dlsym(h, "align64_ctor_ran");
    if (!add || !ctor_ran) {
        printf("dlsym: %s\n", dlerror());
        return 1;
    }
    int a = add(1), b = add(1);
    pthread_t t;
    void* r = NULL;
    if (pthread_create(&t, NULL, thread_fn, NULL) || pthread_join(t, &r)) {
        printf("pthread_create/join failed\n");
        return 1;
    }
    printf("align64_add: %d %d, in a thread %ld, constructor %s\n", a, b, (long)r,
           ctor_ran() ? "ran" : "did not run");
    if (a != 41 || b != 42 || (long)r != 42 || !ctor_ran())
        return 1;
    puts("TEST OK");
    return 0;
}
