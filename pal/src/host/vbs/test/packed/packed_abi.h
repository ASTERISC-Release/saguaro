/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* The tiny PAL table handed to a packed Linux object by the VBS-enclave PAL (phase 2, step a').
 * Shared by the Linux-side test object (built with the GNU toolchain) and the enclave side
 * (clang, Windows target). Fixed-width types only (PLAN.md D4): Linux is LP64, Windows LLP64. */

#ifndef PAL_VBS_PACKED_ABI_H
#define PAL_VBS_PACKED_ABI_H

#include <stdint.h>

struct pal_vbs_packed_table {
    /* write(fd, buf, len) -> bytes written; the enclave logs the bytes through the host console */
    int64_t (*write)(int64_t fd, const char* buf, int64_t len);
};

/* An undefined symbol of the packed object, bound by the in-enclave loader to a function of the
 * enclave (the way the LibOS's imports are bound to the PAL's exports). Returns 3 * x. */
int64_t pal_vbs_import_triple(int64_t x);

/* value packed_entry() returns on success:
 * bytes written + 7 * 1000 + 100 * 1000000 + pal_vbs_import_triple(1000000000) */
#define PAL_VBS_PACKED_EXPECT(bytes_written) \
    ((int64_t)(bytes_written) + 7000 + 100000000 + 3000000000ll)

#endif /* PAL_VBS_PACKED_ABI_H */
