/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * This file contains APIs for miscellaneous use.
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_common.h"
#include "pal_internal.h"

#include "kernel_time.h"
#ifdef PAL_HOST_SEV
#include "sev.h"
#endif

/* No CPU accounting in this PAL (the VM PAL's scheduler would have to keep it). */
int _PalGetCpuTime(bool thread, uint64_t* out_user_us, uint64_t* out_sys_us) {
    __UNUSED(thread);
    __UNUSED(out_user_us);
    __UNUSED(out_sys_us);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalRandomBitsRead(void* buffer, size_t size) {
    return pal_common_random_bits_read(buffer, size);
}

int _PalSystemTimeQuery(uint64_t* out_usec) {
    // log_always("A quick check - 1\n");
    return get_time_in_us(out_usec);
}

int _PalCpuIdRetrieve(uint32_t leaf, uint32_t subleaf, uint32_t values[4]) {
#ifdef __x86_64__
    cpuid(leaf, subleaf, values);
    return 0;
#else
    /* no CPUID instruction on aarch64 (the VM PAL never runs on a CPU exposing CPUID);
     * callers in the guest app get "not implemented" like on any non-Intel platform */
    __UNUSED(leaf);
    __UNUSED(subleaf);
    __UNUSED(values);
    return -PAL_ERROR_NOTIMPLEMENTED;
#endif
}

#ifdef __aarch64__
/* the aarch64 vm kernel is built -mgeneral-regs-only: return the double's bit pattern */
uint64_t _PalGetBogomips(void) {
    return pal_common_get_bogomips();
}
#else
double _PalGetBogomips(void) {
    return pal_common_get_bogomips();
}
#endif

int _PalSegmentBaseGet(enum pal_segment_reg reg, uintptr_t* addr) {
    return pal_common_segment_base_get(reg, addr);
}

int _PalSegmentBaseSet(enum pal_segment_reg reg, uintptr_t addr) {
    return pal_common_segment_base_set(reg, addr);
}

#ifdef PAL_HOST_SEV
/* SEV-SNP: the report and the "quote" are both the firmware-signed attestation report (verified
 * with the chip's VCEK certificate chain from AMD's KDS); target info does not exist (a dummy of
 * one byte, as the LibOS expects a size). */
static int sev_attestation_report(const void* user_report_data, size_t* user_report_data_size,
                                  void* target_info, size_t* target_info_size, void* report,
                                  size_t* report_size) {
    if (!user_report_data_size || !target_info_size || !report_size)
        return -PAL_ERROR_INVAL;
    if (*user_report_data_size != 64 || *target_info_size != 1 ||
            *report_size != SEV_SNP_REPORT_SIZE || !user_report_data) {
        *user_report_data_size = 64;
        *target_info_size = 1;
        *report_size = SEV_SNP_REPORT_SIZE;
        return 0;
    }
    if (target_info)
        memset(target_info, 0, 1);
    if (!report)
        return 0;
    return sev_snp_get_report(user_report_data, report);
}
#endif

int _PalAttestationReport(const void* user_report_data, size_t* user_report_data_size,
                          void* target_info, size_t* target_info_size, void* report,
                          size_t* report_size) {
#ifdef PAL_HOST_SEV
    return sev_attestation_report(user_report_data, user_report_data_size, target_info,
                                  target_info_size, report, report_size);
#endif
    __UNUSED(user_report_data);
    __UNUSED(user_report_data_size);
    __UNUSED(target_info);
    __UNUSED(target_info_size);
    __UNUSED(report);
    __UNUSED(report_size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalAttestationQuote(const void* user_report_data, size_t user_report_data_size, void* quote,
                         size_t* quote_size) {
#ifdef PAL_HOST_SEV
    if (!quote_size || user_report_data_size != 64)
        return -PAL_ERROR_INVAL;
    if (*quote_size < SEV_SNP_REPORT_SIZE || !quote) {
        *quote_size = SEV_SNP_REPORT_SIZE;
        return -PAL_ERROR_NOMEM;
    }
    *quote_size = SEV_SNP_REPORT_SIZE;
    return sev_snp_get_report(user_report_data, quote);
#endif
    __UNUSED(user_report_data);
    __UNUSED(user_report_data_size);
    __UNUSED(quote);
    __UNUSED(quote_size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalGetSpecialKey(const char* name, void* key, size_t* key_size) {
    __UNUSED(name);
    __UNUSED(key);
    __UNUSED(key_size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}
