/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Gramine x64 phase-0 spike (step h): a freestanding x86_64 Linux program (System V ABI). Its only
 * "system call" goes through a function pointer that the loader passes (as a PAL table would). It
 * uses read-only data, initialised data and zero-initialised data, all addressed RIP-relative.
 *
 * Two entries: _start is a plain System V function (the enclave calls it through an assembly thunk
 * that converts from the Windows x64 convention), _start_ms is declared ms_abi so that gcc itself
 * generates the Windows x64 convention (and saves rsi, rdi and xmm6-xmm15). The table's function is
 * Windows code, so the program calls it as ms_abi in both cases. */

struct pal_table {
    long (__attribute__((ms_abi)) *syscall)(long nr, long a0, long a1, long a2);
};

static const char msg[] = "hello from a Linux x86_64 ELF inside a VBS enclave\n";
static int initialised = 7;
static int zeroed;

static long run(struct pal_table* pal)
{
    zeroed += initialised;     /* .bss and .data */
    initialised = 100;
    long r = pal->syscall(1 /* write */, 1, (long)msg, sizeof(msg) - 1);
    long v = r + zeroed * 1000 + initialised * 1000000;   /* expect 51 + 7000 + 100000000 */
    zeroed = 0;
    initialised = 7;
    return v;
}

long _start(struct pal_table* pal) { return run(pal); }

long __attribute__((ms_abi)) _start_ms(struct pal_table* pal) { return run(pal); }
