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

#include <winenclave.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "probe.h"

const IMAGE_ENCLAVE_CONFIG __enclave_config = {
    sizeof(IMAGE_ENCLAVE_CONFIG), IMAGE_ENCLAVE_MINIMUM_CONFIG_SIZE,
    IMAGE_ENCLAVE_POLICY_DEBUGGABLE,    // DO NOT SHIP DEBUGGABLE ENCLAVES TO PRODUCTION
    0, 0, 0, { 0xFE, 0xFE }, { 0x01, 0x02 }, 0, 0,
    0x10000000, 4, IMAGE_ENCLAVE_FLAG_PRIMARY_IMAGE
};

extern unsigned char gx_elf_text[];
extern unsigned char gx_elf_data[];
long long probe_jump(void* fn, long long a0, long long a1);
unsigned long long probe_read_x18(void);

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

// the "system call" entry the Linux program calls through its PAL table. Note: Linux AArch64 is
// LP64 (long is 64 bits), Windows is LLP64 (long is 32 bits), so the boundary uses long long.
static long long pal_syscall(long long nr, long long a0, long long a1, long long a2)
{
    if (nr == 64 && a0 == 1) {
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

static void query(const char* what, void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)))
        logf("  %s %p: protect=0x%lx type=0x%lx\n", what, p, mbi.Protect, mbi.Type);
}

// args[0]: 1 = call the entry through the assembly jump; 2 = through a C function pointer (CFG)
void* CALLBACK E_Elf(void* param)
{
    PROBE_IO* io = (PROBE_IO*)param;
    g_len = 0;
    ELF64_EHDR* eh = (ELF64_EHDR*)gx_elf_text;
    logf("elf: text section %p, data section %p (distance 0x%llx)\n", gx_elf_text, gx_elf_data,
         (unsigned long long)(gx_elf_data - gx_elf_text));
    query("text", gx_elf_text);
    query("data", gx_elf_data);
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->machine != 183) { logf("not an AArch64 ELF\n"); goto out; }
    logf("elf: AArch64 ELF, entry 0x%llx, %u program headers\n", eh->entry, eh->phnum);
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
        void* entry = gx_elf_text + eh->entry;
        unsigned long long x18a = probe_read_x18();
        long long r;
        if (io->args[0] == 2) {
            long long (*fn)(struct pal_table*) = (long long (*)(struct pal_table*))entry;
            logf("elf: calling the entry %p through a C function pointer\n", entry);
            r = fn(&pal);
        } else {
            logf("elf: calling the entry %p through the assembly jump\n", entry);
            r = probe_jump(entry, (long long)&pal, 0);
        }
        logf("elf: the program returned %lld (expected 100007052), x18 %s\n", r, probe_read_x18() == x18a ? "preserved" : "CHANGED");
        io->out[0] = (unsigned long long)r;
    }
out:
    memcpy(io->text, g_log, g_len + 1);
    io->len = g_len;
    return NULL;
}
