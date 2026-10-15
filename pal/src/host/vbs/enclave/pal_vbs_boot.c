/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Boot path of the Windows VBS-enclave PAL (phase 2): the Windows enclave runtime (W) prepares the
 * packed vbs PAL proper ("libpal", an AArch64 Linux ELF packed into this image at build time) with
 * the packed-object loader and enters it with the table of W services (struct pal_vbs_services in
 * pal_vbs_abi.h). The PAL then loads the packed LibOS, which runs the packed application.
 *
 * W keeps every decision about memory protection: the `protect` service refuses any execute
 * permission and any change to the pages of this image outside the packed-images section, so a
 * fault in the Linux code cannot make W's own code or data unusable.
 */

#include <winenclave.h>
#include <stdint.h>
#include <string.h>

#include "pal_vbs_abi.h"
#include "pal_vbs_enclave.h"
#include "pal_vbs_packed.h"

#ifndef PAL_VBS_POOL_SIZE
#define PAL_VBS_POOL_SIZE 0x8000000ull /* 128 MiB of the enclave committed for the PAL */
#endif

extern IMAGE_DOS_HEADER __ImageBase;

static struct pal_vbs_services g_svc;
static struct pal_vbs_ctx g_boot_ctx;
static int64_t g_exit_code;

static void hex(char* out, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = digits[(v >> (60 - 4 * i)) & 0xf];
    out[18] = 0;
}

static void log_kv(const char* key, uint64_t v) {
    char buf[20];
    hex(buf, v);
    pal_vbs_w_log("[vbs-w] ");
    pal_vbs_w_log(key);
    pal_vbs_w_log(" ");
    pal_vbs_w_log(buf);
    pal_vbs_w_log("\n");
}

/* ---- services for the vbs PAL proper (Windows ARM64 functions called from Linux code) ---- */

static void svc_console_write(const char* buf, uint64_t len) {
    while (len) {
        uint64_t n = len > PAL_VBS_CONBUF_SIZE ? PAL_VBS_CONBUF_SIZE : len;
        pal_vbs_w_console_write(buf, n);
        buf += n;
        len -= n;
    }
}

static uint64_t svc_time_ns(void) {
    return pal_vbs_w_time_ns();
}

static int64_t svc_random(void* buf, uint64_t len) {
    return pal_vbs_w_random(buf, len);
}

static int64_t svc_protect(uint64_t addr, uint64_t size, uint64_t prot) {
    if (prot & PAL_VBS_PROT_EXEC)
        return -(int64_t)ERROR_ACCESS_DENIED; /* the platform forbids it; never ask */
    if (!size || addr + size < addr)
        return -(int64_t)ERROR_INVALID_PARAMETER;
    uint64_t end = addr + size;
    uint64_t img_lo = g_svc.image_base, img_hi = g_svc.image_base + g_svc.image_size;
    uint64_t pk_lo = g_svc.pack_base, pk_hi = g_svc.pack_base + g_svc.pack_size;
    if (addr < img_hi && img_lo < end && !(pk_lo <= addr && end <= pk_hi))
        return -(int64_t)ERROR_ACCESS_DENIED; /* this image's own code and data stay as signed */
    DWORD p = (prot & PAL_VBS_PROT_WRITE) ? PAGE_READWRITE
              : (prot & PAL_VBS_PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
    DWORD old = 0;
    if (!VirtualProtect((void*)(uintptr_t)addr, (SIZE_T)size, p, &old))
        return -(int64_t)GetLastError();
    return 0;
}

static void svc_exit(int64_t code) {
    g_exit_code = code;
    pal_vbs_ctx_resume(&g_boot_ctx, 1);
}

int64_t pal_vbs_boot(const struct pal_vbs_shared* shared) {
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)((const uint8_t*)&__ImageBase + __ImageBase.e_lfanew);

    memset(&g_svc, 0, sizeof(g_svc));
    g_svc.version      = PAL_VBS_SERVICES_VERSION;
    g_svc.enclave_base = shared->enclave_base;
    g_svc.enclave_size = shared->enclave_size;
    g_svc.image_base   = (uint64_t)(uintptr_t)&__ImageBase;
    g_svc.image_size   = nt->OptionalHeader.SizeOfImage;
    g_svc.thread_count = shared->thread_count;
    g_svc.objs = pal_vbs_packed_table(&g_svc.nobjs, &g_svc.pack_base, &g_svc.pack_size,
                                      &g_svc.files_base, &g_svc.files_size);

    /* The enclave range comes from the (untrusted) host; it must at least contain this image. */
    if (g_svc.image_base < g_svc.enclave_base ||
        g_svc.image_base + g_svc.image_size > g_svc.enclave_base + g_svc.enclave_size) {
        pal_vbs_w_log("[vbs-w] the host's enclave range does not contain the image\n");
        return -3;
    }

    void* pool = VirtualAlloc(NULL, (SIZE_T)PAL_VBS_POOL_SIZE, MEM_COMMIT | MEM_RESERVE,
                              PAGE_READWRITE);
    if (!pool) {
        log_kv("pool VirtualAlloc failed, error", GetLastError());
        return -4;
    }
    g_svc.pool_base = (uint64_t)(uintptr_t)pool;
    g_svc.pool_size = PAL_VBS_POOL_SIZE;

    log_kv("enclave", g_svc.enclave_base);
    log_kv("image  ", g_svc.image_base);
    log_kv("pack   ", g_svc.pack_base);
    log_kv("pool   ", g_svc.pool_base);

    struct pal_vbs_loaded pal;
    uint64_t detail = 0;
    int64_t ret = pal_vbs_packed_load("libpal", NULL, 0, &pal, &detail);
    if (ret < 0) {
        log_kv("loading libpal failed, error", (uint64_t)ret);
        log_kv("detail", detail);
        return -5;
    }
    if (!pal.entry) {
        pal_vbs_w_log("[vbs-w] libpal has no entry\n");
        return -6;
    }
    log_kv("libpal ", (uint64_t)(uintptr_t)pal.base);
    log_kv("relocs ", pal.nrela);

    g_svc.self_base     = (uint64_t)(uintptr_t)pal.base;
    g_svc.teb           = pal_vbs_read_x18();
    g_svc.console_write = (void*)svc_console_write;
    g_svc.time_ns       = (void*)svc_time_ns;
    g_svc.random        = (void*)svc_random;
    g_svc.protect       = (void*)svc_protect;
    g_svc.exit          = (void*)svc_exit;

    if (pal_vbs_ctx_save(&g_boot_ctx) == 0) {
        /* does not return: the PAL ends through svc_exit, which resumes the saved context */
        pal_vbs_enter_sysv(pal.entry, (uint64_t)(uintptr_t)&g_svc, 0, 0, 0);
        pal_vbs_w_log("[vbs-w] the PAL entry returned\n");
        return -7;
    }
    log_kv("exit code", (uint64_t)g_exit_code);
    return g_exit_code;
}
