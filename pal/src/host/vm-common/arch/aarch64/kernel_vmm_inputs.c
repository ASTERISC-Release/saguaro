/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * aarch64 counterpart of vm-common/kernel_vmm_inputs.c.
 *
 * fw_cfg on QEMU virt is MMIO (hw/arm/virt.c memmap[VIRT_FW_CFG] = 0x09020000, size 0x18), not
 * the legacy 0x510/0x511 port-I/O pair: a 32-bit selector register at +0x00, a 64-bit-accessible
 * data FIFO at +0x08 and DMA at +0x10. The byte-stream semantics and the big-endian file
 * directory format are identical to the x86 interface, so only the three accessor primitives
 * differ; everything below them (argument/env parsing, file lookup) is copied from x86 so both
 * architectures keep seeing exactly the same "opt/saguaro/*" inputs injected by the launcher.
 */

#include <stdint.h>
#include <string.h>

#include "api.h"
#include "pal_error.h"

#include "kernel_time.h"
#include "kernel_vmm_inputs.h"

char g_host_pwd[PATH_MAX];
char g_cmdline[MAX_ARGV_SIZE];
char g_envs[MAX_ENVS_SIZE];

/* QEMU virt's fw_cfg node ("qemu,fw-cfg-mmio", DTB reg = <0x9020000 0x18>). Its register layout
 * is the *inverse* of the legacy x86 i8086 ioport layout (sel@0x510/data@0x511) that the shared
 * vm-common code assumes: hw/arm/virt.c create_fw_cfg() calls
 *     fw_cfg_init_mem_wide(ctl_addr = base + 8, data_addr = base, data_width = 8,
 *                          dma_addr = base + 16, as)
 * so on arm-virt the selector register lives at +8 and the data FIFO at +0. Device contract
 * from hw/nvram/fw_cfg.c:
 *   - ctl (region FW_CFG_CTL_SIZE = 2 bytes @ +8): valid accesses are *only* 2-byte writes
 *     (fw_cfg_ctl_mem_valid: is_write && size == 2). The region is DEVICE_BIG_ENDIAN, so the
 *     selector must reach the device as its MSB on the lower address -- i.e. the guest stores
 *     __builtin_bswap16(selector) little-endian-wise. Reads of any size are rejected
 *     (MEMTX_ERROR -> external abort at EL1).
 *   - data @ +0: 8-byte-wide FIFO; every read pops bytes, byte reads are returned as-is, so a
 *     sequential byte-pop loop (stlrh sel, ldarb data...) is the correct pattern.
 *   - DMA block @ +16: unused here; our fw_cfg files ("opt/saguaro/*") are KB-sized and the
 *     x86 flavor never used DMA on this path either. */
#define FW_CFG_MMIO_BASE   0x09020000UL
#define FW_CFG_MMIO_SEL    (FW_CFG_MMIO_BASE + 0x08) /* u16 big-endian write-only selector */
#define FW_CFG_MMIO_DATA   (FW_CFG_MMIO_BASE + 0x00) /* FIFO, sequential byte pops */

static inline void fw_cfg_select(uint16_t selector) {
    uint16_t wire = __builtin_bswap16(selector);
    __asm__ volatile("stlrh %w0, [%1]" :: "r"((uint32_t)wire),
                     "r"((volatile void*)FW_CFG_MMIO_SEL) : "memory");
}

static inline uint8_t fw_cfg_read_byte(void) {
    uint8_t v;
    __asm__ volatile("ldarb %w0, [%1]" : "=r"(v) : "r"((const volatile void*)FW_CFG_MMIO_DATA)
                     : "memory");
    return v;
}

/* This function copies `input` to a new string (we don't want to modify `input`) and splits the
 * new line in NUL-terminated sub-strings (arguments). Each argument may be enclosed in
 * double-quotes; in this case everything between the double-quotes (including whitespaces) is
 * considered as one argument. Having an argument without a closing double-quote leads to error.
 * No escaping is supported (e.g., `\"` is not allowed).
 * (copied verbatim from the x86 kernel_vmm_inputs.c, where it is a static helper -- the parsing
 * rules must stay byte-identical between architectures). */
static int cmdline_read_common(enum cmdline_parse_type type, const char* input, int* out_cnt,
                               const char** out_array) {
    /* Choose the appropriate starting and ending string as well as the max token size for
     * the specified parse type */
    const char* begin_str;
    const char* end_str;
    size_t max_tokens;
    switch (type) {
        case CMDLINE_ARGS:
            begin_str = GRAMINE_ARGS_BEGIN_STR;
            end_str = GRAMINE_ARGS_END_STR;
            max_tokens = MAX_ARGV_CNT;
            break;
        case CMDLINE_ENVS:
            begin_str = GRAMINE_ENVS_BEGIN_STR;
            end_str = GRAMINE_ENVS_END_STR;
            max_tokens = MAX_ENVS_CNT;
            break;
        default:
            return -PAL_ERROR_INVAL;
    }

    int ret;
    char* p;
    size_t curr_cnt = 0;

    char* input_copy = strdup(input);
    if (!input_copy)
        return -PAL_ERROR_NOMEM;

    p = strstr(input_copy, begin_str);
    if (!p) {
        ret = -PAL_ERROR_INVAL;
        goto out;
    }

    char* p_end = strstr(p, end_str);
    if (!p_end) {
        ret = -PAL_ERROR_INVAL;
        goto out;
    }

    /* do not count the end_str (e.g. `-saguaro-args-end`) and everything after it as arguments */
    *p_end = '\0';

    p += strlen(begin_str);
    while (p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        if (curr_cnt == max_tokens) {
            ret = -PAL_ERROR_NOMEM;
            goto out;
        }

        bool token_in_double_quotes = false;
        if (*p == '"') {
            p++;
            token_in_double_quotes = true;
        }

        out_array[curr_cnt] = p;
        curr_cnt++;

        if (token_in_double_quotes) {
            while (*p != '\0' && *p != '"')
                p++;
            if (*p == '\0') {
                ret = -PAL_ERROR_INVAL;
                goto out;
            }
            *p++ = '\0'; /* replace closing double-quote with NUL */
        } else {
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;
            *p++ = '\0'; /* replace whitespace with NUL */
        }
    }

    /* items of out_array point into `input_copy` buffer */
    *out_cnt = curr_cnt;
    ret = 0;
out:
    if (ret < 0)
        free(input_copy);
    return ret;
}

/* parse the gramine cmdline args passed by the VMM */
int cmdline_read_gramine_args(const char* cmdline, int* out_argc, const char** out_argv) {
    return cmdline_read_common(CMDLINE_ARGS, cmdline, out_argc, out_argv);
}

/* parse the environment variables passed by the VMM */
int cmdline_read_gramine_envs(const char* envs, int* out_envp_cnt, const char** out_envp) {
    return cmdline_read_common(CMDLINE_ENVS, envs, out_envp_cnt, out_envp);
}

static int find_fw_cfg_selector(const char* fw_cfg_name, uint16_t* out_selector,
                                uint32_t* out_size) {
    fw_cfg_select(FW_CFG_FILE_DIR);

    /* the file directory count is 32-bit big-endian in the fw_cfg spec, on all architectures */
    uint8_t fw_cfg_files_count_raw[4];
    for (size_t i = 0; i < sizeof(fw_cfg_files_count_raw); i++)
        fw_cfg_files_count_raw[i] = fw_cfg_read_byte();
    uint32_t fw_cfg_files_count = fw_cfg_files_count_raw[0] + (fw_cfg_files_count_raw[1] << 8)
                                + (fw_cfg_files_count_raw[2] << 16)
                                + (fw_cfg_files_count_raw[3] << 24);
    fw_cfg_files_count = __builtin_bswap32(fw_cfg_files_count);
    if (fw_cfg_files_count > MAX_FW_CFG_FILES)
        return -PAL_ERROR_INVAL;

    uint16_t fw_cfg_selector = 0;
    uint32_t fw_cfg_size = 0;
    for (size_t i = 0; i < fw_cfg_files_count; i++) {
        struct FWCfgFile fw_cfg_file;
        uint8_t* fw_cfg_file_raw = (uint8_t*)&fw_cfg_file;
        for (size_t j = 0; j < sizeof(fw_cfg_file); j++)
            fw_cfg_file_raw[j] = fw_cfg_read_byte();

        if (strlen(fw_cfg_name) + 1 > sizeof(fw_cfg_file.name)) {
            /* make sure the searched-for string is less than the fw_cfg file name limit (56) */
            return -PAL_ERROR_INVAL;
        }

        if (strcmp(fw_cfg_file.name, fw_cfg_name) == 0) {
            fw_cfg_selector = fw_cfg_file.select;
            fw_cfg_size = fw_cfg_file.size;
            break;
        }
    }

    if (!fw_cfg_selector || !fw_cfg_size)
        return -PAL_ERROR_INVAL;

    *out_selector = __builtin_bswap16(fw_cfg_selector);
    *out_size = __builtin_bswap32(fw_cfg_size);
    return 0;
}

static int fw_cfg_read_file(const char* name, char* buf, size_t buf_size) {
    memset(buf, 0, buf_size);

    uint16_t fw_cfg_selector;
    uint32_t fw_cfg_size;
    int ret = find_fw_cfg_selector(name, &fw_cfg_selector, &fw_cfg_size);
    if (ret < 0)
        return ret;

    if (fw_cfg_size >= buf_size)
        return -PAL_ERROR_INVAL;

    fw_cfg_select(fw_cfg_selector);
    for (size_t i = 0; i < fw_cfg_size; i++)
        buf[i] = (char)fw_cfg_read_byte();

    if (strlen(buf) == 0)
        return -PAL_ERROR_INVAL;

    /* note that `buf` is guaranteed to be NULL terminated and have at least one symbol */
    return 0;
}

int cmdline_init_args(char* cmdline_args, size_t cmdline_args_size) {
    int ret = fw_cfg_read_file("opt/saguaro/args", cmdline_args, cmdline_args_size);
    if (ret < 0)
        return ret;
    if (strlen(cmdline_args) >= MAX_ARGV_SIZE)
        return -PAL_ERROR_INVAL;
    return 0;
}

int cmdline_init_envs(char* cmdline_envs, size_t cmdline_envs_size) {
    int ret = fw_cfg_read_file("opt/saguaro/envs", cmdline_envs, cmdline_envs_size);
    if (ret < 0)
        return ret;
    if (strlen(cmdline_envs) >= MAX_ENVS_SIZE)
        return -PAL_ERROR_INVAL;
    return 0;
}

int host_pwd_init(void) {
    return fw_cfg_read_file("opt/saguaro/pwd", g_host_pwd, sizeof(g_host_pwd));
}

int unixtime_init(char* unixtime_s, size_t unixtime_size) {
    int ret = fw_cfg_read_file("opt/saguaro/unixtime_s", unixtime_s, unixtime_size);
    if (ret < 0)
        return ret;
    if (strlen(unixtime_s) >= TIME_S_STR_MAX)
        return -PAL_ERROR_INVAL;
    return 0;
}

/*
 * There is no "etc/e820" fw_cfg blob on arm: the memory map comes from the flattened device
 * tree (/memory node), which is parsed by kernel_dtb.c during early boot. Provided so the
 * shared header's declaration stays satisfiable; nothing in the aarch64 boot path calls it.
 */
int e820_table_init(char* e820_table, size_t* e820_size, size_t max_e820_size) {
    __UNUSED(e820_table);
    __UNUSED(e820_size);
    __UNUSED(max_e820_size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}
