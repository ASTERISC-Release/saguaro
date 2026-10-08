/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*
 * aarch64 port of the x86_64 gramine_entry_api.h.
 *
 * Milestone A (single-threaded stopgap, see pal_arch.h's g_pal_tcb_singleton comment) does not
 * patch any libc for aarch64 -- all syscall interception goes through the generic seccomp-trap
 * path (maybe_emulate_syscall() in libos_context.c), not through this macro. This header still
 * has to exist and assemble/compile correctly, though: GRAMINE_SYSCALL_OFFSET is checked by a
 * static_assert in libos_tcb.h regardless of architecture, and gramine_call() below is still
 * compiled (even though unused) wherever this header is included, so an x86-only inline-asm body
 * here would simply fail to assemble on aarch64.
 *
 * The aarch64 GRAMINE_SYSCALL macro below is written to be genuinely correct, not just a stub,
 * so that wiring up a real patched-libc fast path later (Milestone B territory) is a matter of
 * pointing a libc's syscall macro at it, not revisiting this file.
 */

#ifndef GRAMINE_ENTRY_API_H_
#define GRAMINE_ENTRY_API_H_

/* Offset of `libos_syscall_entry` inside the combined PAL_TCB+libos_tcb_t blob (checked against
 * offsetof(PAL_TCB, libos_tcb) + offsetof(libos_tcb_t, libos_syscall_entry) by a static_assert in
 * libos_tcb.h). This is a pure layout fact, not an x86_64-specific value -- PAL_TCB's layout is
 * the same shape on aarch64 (see pal_arch.h), so the number is unchanged. */
#define GRAMINE_SYSCALL_OFFSET 24

#ifdef __ASSEMBLER__

/*
 * Unlike x86_64 (which reaches PAL_TCB via %gs, a register with no real hardware-return-address
 * convention to mimic, since the real `syscall` instruction itself clobbers rcx/r11 as part of
 * its own SYSCALL/SYSRET contract that this macro must match from the caller's point of view),
 * aarch64 has no such convention to replicate: `svc` traps are handled entirely inside the
 * kernel's exception entry/exit, invisible to the caller. So this can be a perfectly ordinary
 * AAPCS64 call (`blr`, return via the callee's `ret`) rather than a hand-rolled register-based
 * return scheme. X16/X17 are the architecture's own designated intra-procedure-call scratch
 * registers (used identically by linker veneers/PLT stubs), so using X16 here clobbers nothing
 * a caller could be relying on.
 */
.macro GRAMINE_SYSCALL
    adrp x16, g_pal_tcb_singleton
    ldr  x16, [x16, :lo12:g_pal_tcb_singleton]
    ldr  x16, [x16, GRAMINE_SYSCALL_OFFSET]
    blr  x16
.endm

#else /* !__ASSEMBLER__ */

#include <stdbool.h>
#include <stdint.h>

#define GRAMINE_STR(x)  #x
#define GRAMINE_XSTR(x) GRAMINE_STR(x)

__asm__(
    ".macro GRAMINE_SYSCALL\n"
    "adrp x16, g_pal_tcb_singleton\n"
    "ldr  x16, [x16, :lo12:g_pal_tcb_singleton]\n"
    "ldr  x16, [x16, " GRAMINE_XSTR(GRAMINE_SYSCALL_OFFSET) "]\n"
    "blr  x16\n"
    ".endm\n"
);

/* Magic syscall number for Gramine custom calls. aarch64 keeps the "which call is this" number
 * in X8 (separate from the X0-X2 arguments), mirroring how real aarch64 syscalls keep the
 * syscall number in X8 separate from the X0-X5 arguments -- unlike x86_64, which reuses %eax for
 * both the real syscall number and this custom-call number. */
#define GRAMINE_CUSTOM_SYSCALL_NR 0x100000

/* GCC does not support `__attribute__((naked))` on aarch64 (unlike x86_64, where that's how the
 * body below is kept as pure, prologue-free asm) -- so, unlike the x86_64 version, this is a
 * perfectly ordinary function with a normal AAPCS64 prologue/epilogue. Declaring the clobbers
 * (including X30/LR, since GRAMINE_SYSCALL's `blr` overwrites it) lets the compiler save/restore
 * whatever it needs around the asm block on its own, and binding the output to X0 explicitly
 * (rather than relying on it surviving untouched into an implicit fallthrough return, which
 * would also trip -Wreturn-type) makes this correct under -Werror too. */
static int gramine_call(int number __attribute__((unused)),
                        unsigned long arg1 __attribute__((unused)),
                        unsigned long arg2 __attribute__((unused))) {
    register long ret_val __asm__("x0");
    __asm__ volatile (
        "mov x8, #" GRAMINE_XSTR(GRAMINE_CUSTOM_SYSCALL_NR) "\n"
        "GRAMINE_SYSCALL\n"
        : "=r"(ret_val)
        :
        : "x8", "x16", "x30", "memory"
    );
    return (int)ret_val;
}

#undef GRAMINE_XSTR
#undef GRAMINE_STR

/* Custom call numbers */
enum {
    GRAMINE_CALL_REGISTER_LIBRARY = 1,
    GRAMINE_CALL_RUN_TEST,
    /* For RW locks test. GRAMINE_CALL_RUN_TEST is not suitable for it, because we need full control
     * over locks from multiple threads. Additionally, this test is non-trivial in size, so it
     * doesn't seem right to always compile it into our LibOS. */
    GRAMINE_CALL_RWLOCK_CREATE,
    GRAMINE_CALL_RWLOCK_DESTROY,
    GRAMINE_CALL_RWLOCK_READ_LOCK,
    GRAMINE_CALL_RWLOCK_READ_UNLOCK,
    GRAMINE_CALL_RWLOCK_WRITE_LOCK,
    GRAMINE_CALL_RWLOCK_WRITE_UNLOCK,
};

static inline int gramine_register_library(const char* name, unsigned long load_address) {
    return gramine_call(GRAMINE_CALL_REGISTER_LIBRARY, (unsigned long)name, load_address);
}

static inline int gramine_run_test(const char* test_name) {
    return gramine_call(GRAMINE_CALL_RUN_TEST, (unsigned long)test_name, 0);
}

static inline bool gramine_rwlock_create(void** out_lock) {
    return gramine_call(GRAMINE_CALL_RWLOCK_CREATE, (unsigned long)out_lock, 0);
}

static inline void gramine_rwlock_destroy(void* lock) {
    gramine_call(GRAMINE_CALL_RWLOCK_DESTROY, (unsigned long)lock, 0);
}

static inline void gramine_rwlock_read_lock(void* lock) {
    gramine_call(GRAMINE_CALL_RWLOCK_READ_LOCK, (unsigned long)lock, 0);
}

static inline void gramine_rwlock_read_unlock(void* lock) {
    gramine_call(GRAMINE_CALL_RWLOCK_READ_UNLOCK, (unsigned long)lock, 0);
}

static inline void gramine_rwlock_write_lock(void* lock) {
    gramine_call(GRAMINE_CALL_RWLOCK_WRITE_LOCK, (unsigned long)lock, 0);
}

static inline void gramine_rwlock_write_unlock(void* lock){
    gramine_call(GRAMINE_CALL_RWLOCK_WRITE_UNLOCK, (unsigned long)lock, 0);
}

#endif /* __ASSEMBLER__ */

#endif /* GRAMINE_ENTRY_API_H_ */
