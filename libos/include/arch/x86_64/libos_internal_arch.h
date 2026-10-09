/* SPDX-License-Identifier: LGPL-3.0-or-later */

#pragma once

#define LIBOS_ELF_HOST_MACHINE EM_X86_64

#include <stdbool.h>
#include <stdint.h>

#include "pal.h"

/* SIGILL handling hooks used by illegal_upcall() (see the aarch64 versions for their purpose).
 * Nothing on x86_64 needs special treatment. */
static inline bool libos_arch_is_intentional_internal_insn(uintptr_t addr) {
    (void)addr;
    return false;
}

static inline bool libos_arch_repair_illegal_insn(PAL_CONTEXT* context) {
    (void)context;
    return false;
}

/* aarch64 restart mirror hook (see the aarch64 version): not needed on x86_64. */
static inline void libos_arch_save_restart_arg0(PAL_CONTEXT* context) {
    (void)context;
}
