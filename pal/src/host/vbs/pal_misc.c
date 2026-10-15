/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Time, randomness and CPU information of the vbs PAL proper. */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

int _PalSystemTimeQuery(uint64_t* out_usec) {
    /* host wall clock through an OCALL: untrusted, as in the SGX PAL */
    *out_usec = vbs_time_ns() / 1000;
    return 0;
}

int _PalGetCpuTime(bool thread, uint64_t* out_user_us, uint64_t* out_sys_us) {
    __UNUSED(thread);
    __UNUSED(out_user_us);
    __UNUSED(out_sys_us);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalRandomBitsRead(void* buffer, size_t size) {
    /* BCryptGenRandom inside the enclave */
    return vbs_random(buffer, size) == 0 ? 0 : -PAL_ERROR_DENIED;
}

int _PalSegmentBaseGet(enum pal_segment_reg reg, uintptr_t* addr) {
    __UNUSED(reg);
    __UNUSED(addr);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalSegmentBaseSet(enum pal_segment_reg reg, uintptr_t addr) {
    __UNUSED(reg);
    __UNUSED(addr);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalCpuIdRetrieve(uint32_t leaf, uint32_t subleaf, uint32_t values[4]) {
    __UNUSED(leaf);
    __UNUSED(subleaf);
    __UNUSED(values);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalAttestationReport(const void* user_report_data, size_t* user_report_data_size,
                          void* target_info, size_t* target_info_size, void* report,
                          size_t* report_size) {
    __UNUSED(user_report_data);
    __UNUSED(user_report_data_size);
    __UNUSED(target_info);
    __UNUSED(target_info_size);
    __UNUSED(report);
    __UNUSED(report_size);
    return -PAL_ERROR_NOTIMPLEMENTED; /* phase 4 */
}

int _PalAttestationQuote(const void* user_report_data, size_t user_report_data_size, void* quote,
                         size_t* quote_size) {
    __UNUSED(user_report_data);
    __UNUSED(user_report_data_size);
    __UNUSED(quote);
    __UNUSED(quote_size);
    return -PAL_ERROR_NOTIMPLEMENTED; /* phase 4 */
}

int _PalGetSpecialKey(const char* name, void* key, size_t* key_size) {
    __UNUSED(name);
    __UNUSED(key);
    __UNUSED(key_size);
    return -PAL_ERROR_NOTIMPLEMENTED; /* phase 4 (sealing) */
}

double _PalGetBogomips(void) {
    return 0.0;
}
