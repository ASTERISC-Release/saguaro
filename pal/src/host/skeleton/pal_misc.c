/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains APIs for miscellaneous use.
 */

#include "api.h"
#include "assert.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"

int _PalSystemTimeQuery(uint64_t* out_usec) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

/* No CPU accounting in this PAL (the VM PAL's scheduler would have to keep it). */
int _PalGetCpuTime(bool thread, uint64_t* out_user_us, uint64_t* out_sys_us) {
    __UNUSED(thread);
    __UNUSED(out_user_us);
    __UNUSED(out_sys_us);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalRandomBitsRead(void* buffer, size_t size) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalSegmentBaseGet(enum pal_segment_reg reg, uintptr_t* addr) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalSegmentBaseSet(enum pal_segment_reg reg, uintptr_t addr) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalSegmentDescriptorSet(unsigned int entry, uint64_t descriptor) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalSegmentDescriptorGet(unsigned int entry, uint64_t* descriptor) {
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalCpuIdRetrieve(uint32_t leaf, uint32_t subleaf, uint32_t values[4]) {
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
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalAttestationQuote(const void* user_report_data, size_t user_report_data_size, void* quote,
                         size_t* quote_size) {
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

double _PalGetBogomips(void) {
    /* this has to be implemented */
    return 0.0;
}
