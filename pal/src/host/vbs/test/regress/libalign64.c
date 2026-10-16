/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The shared object that dlopen_align64 loads: linked with 64 KiB PT_LOAD alignment (build.sh), as
 * the AArch64 libraries of the distributions are (libgcc_s, libstdc++). It has what libgcc_s has at
 * load time: a constructor that calls libc through a lazily bound PLT entry, and thread-local data
 * (TLS descriptors of a library loaded after startup). Documentation/devel/vbs-enclave/
 * PLAN-packer-align.md. */

#include <stdlib.h>
#include <sys/auxv.h>

static __thread int tls_counter = 40;
static unsigned long g_hwcap;

__attribute__((constructor)) static void init(void) {
    g_hwcap = getauxval(AT_HWCAP); /* PLT, bound lazily: _dl_fixup runs here */
}

int align64_add(int x) {
    tls_counter += x;
    return tls_counter;
}

int align64_ctor_ran(void) {
    return g_hwcap != 0;
}
