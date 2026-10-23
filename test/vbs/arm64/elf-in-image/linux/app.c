/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Saguaro phase-0 spike (step h): a freestanding AArch64 Linux program. Its only "system call"
 * goes through a function pointer that the loader passes in x0 (as a PAL table would). It uses
 * read-only data, initialised data and zero-initialised data, all addressed PC-relative. */

struct pal_table {
    long (*syscall)(long nr, long a0, long a1, long a2);
};

static const char msg[] = "hello from a Linux AArch64 ELF inside a VBS enclave\n";
static int initialised = 7;
static int zeroed;

long _start(struct pal_table* pal)
{
    zeroed += initialised;     /* .bss and .data */
    initialised = 100;
    long r = pal->syscall(64 /* write */, 1, (long)msg, sizeof(msg) - 1);
    return r + zeroed * 1000 + initialised * 1000000;   /* expect 52 + 7000 + 100000000 */
}
