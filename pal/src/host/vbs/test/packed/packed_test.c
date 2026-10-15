/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * A real position-independent AArch64 *Linux* shared object that the phase-2 packer embeds verbatim
 * (its whole load image) into the signed VBS enclave image, and that the PAL's in-enclave loader
 * makes writable where needed, relocates at run time and calls -- the (a') experiment of PLAN.md D2.
 *
 * It exercises what Gramine's loaders will face with the LibOS and glibc:
 *   - .rodata, initialised .data and .bss, all PC-relative;
 *   - a default-visibility global reached through the GOT     (R_AARCH64_GLOB_DAT / ABS64);
 *   - a data pointer to that global                            (R_AARCH64_ABS64 with a symbol);
 *   - a function-pointer table                                 (R_AARCH64_RELATIVE);
 *   - a call to a default-visibility function through the PLT (R_AARCH64_JUMP_SLOT, -z now);
 *   - a call OUT to the PAL through a function pointer (a stand-in for a system call / OCALL).
 * Built with -ffixed-x18 (the register is the enclave TEB) and -nostdlib; see build.sh.
 */

#include <stdint.h>
#include "packed_abi.h"

static const char msg[] = "hello from a packed PIC shared object inside a VBS enclave\n";

int packed_global = 7;                       /* GOT-accessed (default visibility) */
int* const packed_global_ptr = &packed_global; /* R_AARCH64_ABS64 against the symbol */
static int zeroed;                           /* .bss */

static int64_t add(int64_t a, int64_t b) { return a + b; }
static int64_t mul(int64_t a, int64_t b) { return a * b; }
int64_t (*const ops[2])(int64_t, int64_t) = { add, mul }; /* R_AARCH64_RELATIVE x2 */

int64_t packed_helper(int64_t x) { return x; }   /* called through the PLT (JUMP_SLOT) */

int64_t packed_entry(struct pal_vbs_packed_table* pal) {
    zeroed += *packed_global_ptr;            /* .bss += GOT/ABS64 global (7) */
    *packed_global_ptr = 100;                /* write through the relocated pointer */
    int64_t w = pal->write(1, msg, (int64_t)(sizeof(msg) - 1));
    int64_t r = ops[1](zeroed, 1000);        /* 7 * 1000 via a RELATIVE-relocated pointer */
    r = ops[0](r, packed_helper(packed_global) * 1000000); /* + 100 * 1000000 via the PLT */
    return r + w;                            /* == PAL_VBS_PACKED_EXPECT(w) */
}
