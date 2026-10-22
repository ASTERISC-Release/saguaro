//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
//
// Saguaro phase-0 spike (step h): runs a Linux AArch64 ELF program that was placed into the signed
// enclave image at build time (elf.asm). The enclave configuration comes from Microsoft's
// VbsEnclave sample (MIT).
//
// x64 version: the program is an x86_64 System V ELF placed by elf_blob.c (linux/elfsplit.py); it is
// entered either through an assembly thunk (thunk.asm, Windows x64 to System V) or through an entry
// that gcc compiled as ms_abi. The x64 changes: Copyright (C) 2026 The ASTERISC Research Lab

#include <winenclave.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "probe.h"
#include "elf_blob.h"

const IMAGE_ENCLAVE_CONFIG __enclave_config = {
    sizeof(IMAGE_ENCLAVE_CONFIG), IMAGE_ENCLAVE_MINIMUM_CONFIG_SIZE,
    IMAGE_ENCLAVE_POLICY_DEBUGGABLE,    // DO NOT SHIP DEBUGGABLE ENCLAVES TO PRODUCTION
    0, 0, 0, { 0xFE, 0xFE }, { 0x01, 0x02 }, 0, 0,
    0x10000000, 4, IMAGE_ENCLAVE_FLAG_PRIMARY_IMAGE
};

extern const unsigned char gx_elf_text[];
extern unsigned char gx_elf_data[];
long long call_sysv(const void* fn, long long a0);   // thunk.asm
unsigned long long read_callee_saved_sum(void);       // thunk.asm: rbx+rbp+rsi+rdi+r12..r15

static char g_log[PROBE_TEXT];
static int g_len;
static void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_log + g_len, sizeof(g_log) - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len += n;
    if (g_len > (int)sizeof(g_log) - 1) g_len = sizeof(g_log) - 1;
}

BOOL DllMain(HINSTANCE h, DWORD r, LPVOID p) { UNREFERENCED_PARAMETER(h); UNREFERENCED_PARAMETER(r); UNREFERENCED_PARAMETER(p); return TRUE; }

// the "system call" entry the Linux program calls through its PAL table (the program declares it
// ms_abi). Note: Linux x86_64 is LP64 (long is 64 bits), Windows is LLP64 (long is 32 bits), so the
// boundary uses long long.
static long long pal_syscall(long long nr, long long a0, long long a1, long long a2)
{
    if (nr == 1 && a0 == 1) {
        logf("pal_syscall: write(1, \"%.*s\")", (int)a2, (const char*)a1);
        return a2;
    }
    logf("pal_syscall: nr=%lld unsupported\n", nr);
    return -38;
}

struct pal_table { long long (*syscall)(long long, long long, long long, long long); };

typedef struct { unsigned char ident[16]; unsigned short type, machine; unsigned int version;
                 unsigned long long entry, phoff, shoff; unsigned int flags;
                 unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ELF64_EHDR;
typedef struct { unsigned int type, flags; unsigned long long offset, vaddr, paddr, filesz, memsz, align; } ELF64_PHDR;

static void query(const char* what, const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)))
        logf("  %s %p: protect=0x%lx type=0x%lx\n", what, p, mbi.Protect, mbi.Type);
}

// args[0]: 1 = call _start (System V) through the assembly thunk; 2 = call _start_ms (ms_abi)
// through a C function pointer (CFG may check it)
void* CALLBACK E_Elf(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    g_len = 0;
    ELF64_EHDR* eh = (ELF64_EHDR*)gx_elf_text;
    logf("elf: text section %p, data section %p (distance 0x%llx)\n", gx_elf_text, gx_elf_data,
         (unsigned long long)(gx_elf_data - gx_elf_text));
    query("text", gx_elf_text);
    query("data", gx_elf_data);
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->machine != 62) { logf("not an x86_64 ELF\n"); goto out; }
    logf("elf: x86_64 ELF, entry 0x%llx, %u program headers\n", eh->entry, eh->phnum);
    for (int i = 0; i < eh->phnum; i++) {
        ELF64_PHDR* ph = (ELF64_PHDR*)(gx_elf_text + eh->phoff + i * eh->phentsize);
        if (ph->type != 1) continue;
        logf("  PT_LOAD flags=%u vaddr=0x%llx memsz=0x%llx\n", ph->flags, ph->vaddr, ph->memsz);
        if ((ph->flags & 2) && ph->vaddr != (unsigned long long)(gx_elf_data - gx_elf_text)) {
            logf("elf: the data segment is not where the program expects it; cannot run\n");
            goto out;
        }
    }
    {
        struct pal_table pal = { pal_syscall };
        unsigned long long cs = read_callee_saved_sum();
        long long r;
        if (io->args[0] == 2) {
            long long (*fn)(struct pal_table*) = (long long (*)(struct pal_table*))(gx_elf_text + ELF_START_MS);
            logf("elf: calling _start_ms %p (ms_abi) through a C function pointer\n", (void*)fn);
            r = fn(&pal);
        } else {
            const void* entry = gx_elf_text + ELF_START;
            logf("elf: calling _start %p (System V) through the assembly thunk\n", entry);
            r = call_sysv(entry, (long long)&pal);
        }
        logf("elf: the program returned %lld (expected 100007051), callee-saved registers %s\n", r,
             read_callee_saved_sum() == cs ? "preserved" : "CHANGED");
        io->out[0] = (unsigned long long)r;
    }
out:
    memcpy(io->text, g_log, g_len + 1);
    io->len = g_len;
    return NULL;
}
