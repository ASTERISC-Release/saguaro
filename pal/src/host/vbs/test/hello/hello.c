/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The phase-2 gate of the VBS-enclave PAL: an ordinary glibc program, statically linked as a PIE,
 * packed into the signed enclave image and run under the Gramine LibOS inside the enclave. */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    printf("Hello, world! (from a Linux program under Gramine inside a VBS enclave)\n");
    printf("argc=%d argv[0]=%s pid=%d\n", argc, argc > 0 ? argv[0] : "?", (int)getpid());
    return 42;
}
