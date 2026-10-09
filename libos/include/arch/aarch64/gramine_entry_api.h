/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
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
 * aarch64 redesign of the x86_64 macro (see the file comment above for why aarch64 has no
 * SYSCALL/SYSRET-style register contract to mimic): the entry point must NOT be called with a
 * plain `blr`. libos_syscall_entry's contract carries the resume address in X16 (see syscallas.S
 * for why X30 must not be that carrier -- aarch64 `svc` destroys no registers, so X30 at a
 * syscall site is the application's own live link register, and clobbering it corrupts the
 * caller's return path). A macro at a syscall site can only leave its follow-on address where it
 * can name it, so this uses the other half of the sanctioned IP-scratch pair: `adr x16, 1f`
 * plants the resume address explicitly and `br` (not `blr`) transfers control without touching
 * X30; _return_from_syscall() finishes by branching to the saved resume. X30 survives exactly as
 * it would across a real `svc`.
 *
 * The GOT chain runs through X17 (X16 must stay intact holding the resume address once planted).
 * It is a GOT (not direct :lo12:) reference to g_pal_tcb_singleton, since any caller of this
 * macro -- whether PAL/LibOS code or, eventually, a patched libc -- lives in a different shared
 * object from libpal.so, where g_pal_tcb_singleton is actually defined; a direct adrp/:lo12:
 * pair computes a link-time-fixed offset and fails to link ("relocation ... may bind
 * externally"). The final `ldr [x17]` is needed because the GOT slot holds the *address* of the
 * variable, not its value (itself a pointer, PAL_TCB*).
 */
.macro GRAMINE_SYSCALL
    adr  x16, 1f
    adrp x17, :got:g_pal_tcb_singleton
    ldr  x17, [x17, :got_lo12:g_pal_tcb_singleton]
    ldr  x17, [x17]
    ldr  x17, [x17, GRAMINE_SYSCALL_OFFSET]
    br   x17
1:
.endm

#else /* !__ASSEMBLER__ */

#include <stdbool.h>
#include <stdint.h>

#define GRAMINE_STR(x)  #x
#define GRAMINE_XSTR(x) GRAMINE_STR(x)

__asm__(
    ".macro GRAMINE_SYSCALL\n"
    "adr  x16, 1f\n"
    "adrp x17, :got:g_pal_tcb_singleton\n"
    "ldr  x17, [x17, :got_lo12:g_pal_tcb_singleton]\n"
    "ldr  x17, [x17]\n"
    "ldr  x17, [x17, " GRAMINE_XSTR(GRAMINE_SYSCALL_OFFSET) "]\n"
    "br   x17\n"
    "1:\n"
    ".endm\n"
);

/* Magic syscall number for Gramine custom calls. aarch64 keeps the "which call is this" number
 * in X8 (separate from the X0-X2 arguments), mirroring how real aarch64 syscalls keep the
 * syscall number in X8 separate from the X0-X5 arguments -- unlike x86_64, which reuses %eax for
 * both the real syscall number and this custom-call number. */
#define GRAMINE_CUSTOM_SYSCALL_NR 0x100000

/*
 * Unlike the x86_64 version (and unlike GRAMINE_SYSCALL above), this issues a plain `svc #0`
 * rather than going through GRAMINE_SYSCALL. The macro needs g_pal_tcb_singleton, which is only
 * resolvable from within PAL/LibOS's own shared objects (see its definition in
 * pal_arch_exception.c) -- but gramine_call() is called from *guest application* code (this
 * header is also meant for patched libc/test programs to use), a separate, unrelated binary that
 * has no link relationship to libpal.so at all. A raw `svc` works correctly instead: it traps via
 * the same seccomp filter as every other syscall under Milestone A's design (no libc patching),
 * lands in libos_syscall_entry via maybe_emulate_syscall(), and libos_syscalls.c already checks
 * the incoming syscall number against GRAMINE_CUSTOM_SYSCALL_NR generically (architecture-
 * independent) to route it to handle_libos_call() -- so this needs no special-casing on the
 * LibOS side at all, just the right number in X8 here.
 *
 * X0/X1/X2 already hold number/arg1/arg2 exactly where handle_libos_call(args[0], args[1],
 * args[2]) expects them (ALL_SYSCALL_ARGS pulls straight from context->regs[0..5]), so, unlike
 * x86_64's version, no register shuffling is needed beyond setting X8 -- and because this ends
 * in a normal C `return` (not a bare asm block standing in for the whole function body the way
 * x86_64's naked version does), there's no -Wreturn-type concern to work around either.
 */
static int gramine_call(int number, unsigned long arg1, unsigned long arg2) {
    register long x0 __asm__("x0") = number;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x8 __asm__("x8") = GRAMINE_CUSTOM_SYSCALL_NR;
    __asm__ volatile(
        "svc #0\n"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory"
    );
    return (int)x0;
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
