/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Time, randomness and CPU information of the vbs PAL proper. */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_linux_error.h"
#include "pal_vbs.h"

int _PalSystemTimeQuery(uint64_t* out_usec) {
    /* host wall clock through an OCALL: untrusted, as in the SGX PAL */
    *out_usec = vbs_time_ns() / 1000;
    return 0;
}

int _PalGetCpuTime(bool thread, uint64_t* out_user_us, uint64_t* out_sys_us) {
    /* the host's accounting of the calling host thread (which runs this enclave thread) or of the
     * host process: untrusted, like the time */
    uint64_t t[2] = {0, 0};
    size_t n = 0;
    int64_t r = vbs_hostcall(PAL_VBS_HOST_CPUTIME, thread ? 1 : 0, 0, 0, 0, NULL, 0, t, sizeof(t),
                             &n);
    if (r < 0)
        return unix_to_pal_error((int)r);
    if (n != sizeof(t))
        return -PAL_ERROR_DENIED;
    *out_user_us = t[0];
    *out_sys_us = t[1];
    return 0;
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

/*
 * Attestation (PLAN.md, phase 4): the VBS attestation report package of EnclaveGetAttestationReport,
 * signed by the platform, with the 64 bytes of user report data as its EnclaveData. The report is
 * already what a verifier checks, so the quote is the same package (D4.1); VBS has no target info,
 * so like TDX the PAL reports a dummy one.
 */
#define VBS_USER_REPORT_DATA_SIZE 64
static const char g_vbs_target_info_dummy[] = "vbs: no target info";

static int64_t vbs_report_size(void) {
    static int64_t size;
    if (!size) {
        uint8_t zero[VBS_USER_REPORT_DATA_SIZE] = {0};
        size = vbs_attest(zero, NULL, 0);
    }
    return size;
}

int _PalAttestationReport(const void* user_report_data, size_t* user_report_data_size,
                          void* target_info, size_t* target_info_size, void* report,
                          size_t* report_size) {
    if (!user_report_data_size || !target_info_size || !report_size)
        return -PAL_ERROR_INVAL;
    int64_t size = vbs_report_size();
    if (size <= 0 || size > PAL_VBS_REPORT_MAX)
        return -PAL_ERROR_DENIED;

    if (*user_report_data_size == VBS_USER_REPORT_DATA_SIZE &&
            *target_info_size == sizeof(g_vbs_target_info_dummy) &&
            *report_size == (size_t)size && user_report_data) {
        if (report) {
            int64_t got = vbs_attest(user_report_data, report, (size_t)size);
            if (got != size)
                return -PAL_ERROR_DENIED;
        }
        if (target_info)
            memcpy(target_info, g_vbs_target_info_dummy, sizeof(g_vbs_target_info_dummy));
    }
    *user_report_data_size = VBS_USER_REPORT_DATA_SIZE;
    *target_info_size = sizeof(g_vbs_target_info_dummy);
    *report_size = (size_t)size;
    return 0;
}

int _PalAttestationQuote(const void* user_report_data, size_t user_report_data_size, void* quote,
                         size_t* quote_size) {
    if (user_report_data_size != VBS_USER_REPORT_DATA_SIZE || !quote_size)
        return -PAL_ERROR_INVAL;
    int64_t size = vbs_report_size();
    if (size <= 0 || size > PAL_VBS_REPORT_MAX)
        return -PAL_ERROR_DENIED;
    if (*quote_size < (size_t)size) {
        *quote_size = (size_t)size;
        return -PAL_ERROR_NOMEM;
    }
    int64_t got = vbs_attest(user_report_data, quote, (size_t)size);
    if (got != size)
        return -PAL_ERROR_DENIED;
    *quote_size = (size_t)size;
    return 0;
}

int _PalGetSpecialKey(const char* name, void* key, size_t* key_size) {
    __UNUSED(name);
    __UNUSED(key);
    __UNUSED(key_size);
    return -PAL_ERROR_NOTIMPLEMENTED; /* sealing: a future item (TODO.md) */
}

double _PalGetBogomips(void) {
    return 0.0;
}
