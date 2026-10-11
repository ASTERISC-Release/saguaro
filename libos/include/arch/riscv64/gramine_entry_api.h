/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*
 * riscv64 port of the x86_64 gramine_entry_api.h.
 *
 * The riscv64 direct-PAL port does not patch any libc -- all syscall interception goes through
 * the generic seccomp-trap path (maybe_emulate_syscall() in libos_context.c), not through this
 * macro (Documentation/devel/riscv/plan.md decisions). This header still has to exist and
 * assemble/compile correctly, though: GRAMINE_SYSCALL_OFFSET is checked by a static_assert in
 * libos_tcb.h regardless of architecture, and gramine_call() below is still compiled (even
 * though unused) wherever this header is included, so an x86-only inline-asm body here would
 * simply fail to assemble on riscv64.
 *
 * The riscv64 GRAMINE_SYSCALL macro below is written to be genuinely correct, not just a stub,
 * so that wiring up a real patched-libc fast path later is a matter of pointing a libc's syscall
 * macro at it, not revisiting this file.
 */

#ifndef GRAMINE_ENTRY_API_H_
#define GRAMINE_ENTRY_API_H_

/* Offset of `libos_syscall_entry` inside the combined PAL_TCB+libos_tcb_t blob (checked against
 * offsetof(PAL_TCB, libos_tcb) + offsetof(libos_tcb_t, libos_syscall_entry) by a static_assert in
 * libos_tcb.h). This is a pure layout fact, not an x86_64-specific value -- PAL_TCB's layout is
 * the same shape on riscv64 (see pal_arch.h), so the number is unchanged. */
#define GRAMINE_SYSCALL_OFFSET 24

#ifdef __ASSEMBLER__

/*
 * riscv64 redesign of the x86_64/aarch64 macro. Entry contract: like aarch64, the entry point
 * receives the *resume address* in a linker-reserved scratch pair member (a6/a7 are IP0/IP1 over
 * there; riscv64 has no such registers -- t0/t1 are the conventional call-clobbered scratch
 * pair) rather than a link register that the application's own `ret` needs: riscv64's `ecall`
 * destroys nothing, so ra at a syscall site is the application's own live link register
 * (aarch64 finding #1 applies verbatim). The syscall number must already be in a7.
 *
 * RISC-V's `auipc`-style PC-relative materialization of a *local label* (%pcrel_hi/%pcrel_lo
 * pair) cannot be written portably by hand -- the linker's %pcrel_lo matching rejects
 * hand-spelled distant label pairs ("dangerous relocation") -- so the resume address is computed
 * as `auipc t0, 0` (pc, no relocation at all) plus a constant `addi` over the macro's own fixed
 * length. `.option norelax` pins that length: under it the assembler expands
 * `la t1, <dynamic symbol>` to exactly two instructions (GOT-slot load), making the whole macro
 * exactly six 4-byte instructions with the resume site at +24 from the `auipc`. The label below
 * marks that site; an assembler that ever breaks the norelax size guarantee would break this
 * macro too -- the static test for that is the `sigreturn_regs`-style harness, not a
 * compile-time check (patched-libc fast path is deferred anyway, see the file comment).
 */
.macro GRAMINE_SYSCALL
    .option push
    .option norelax
    auipc t0, 0            /* t0 = pc of this auipc */
    addi  t0, t0, 24       /* t0 = address of the `1:` resume site (see the size comment above) */
    la    t1, g_pal_tcb_singleton  /* t1 = this thread's PAL_TCB (GOT load at link time) */
    ld    t1, GRAMINE_SYSCALL_OFFSET(t1)  /* t1 = the libos_syscall_entry pointer */
    jr    t1
1:
    .option pop
.endm

#else /* !__ASSEMBLER__ */

#include <stdbool.h>
#include <stdint.h>

#define GRAMINE_STR(x)  #x
#define GRAMINE_XSTR(x) GRAMINE_STR(x)

__asm__(
    ".macro GRAMINE_SYSCALL\n"
    ".option push\n"
    ".option norelax\n"
    "auipc t0, 0\n"
    "addi t0, t0, 24\n"
    "la t1, g_pal_tcb_singleton\n"
    "ld t1, " GRAMINE_XSTR(GRAMINE_SYSCALL_OFFSET) "(t1)\n"
    "jr t1\n"
    "1:\n"
    ".option pop\n"
    ".endm\n"
);

/* Magic syscall number for Gramine custom calls. riscv64 keeps the "which call is this" number
 * in a7 (separate from the a0-a2 arguments), mirroring how real riscv64 syscalls keep the number
 * in a7 separate from the a0-a5 arguments -- unlike x86_64, which reuses %eax for both (the
 * aarch64 sibling keeps it in x8 for the same reason). */
#define GRAMINE_CUSTOM_SYSCALL_NR 0x100000

/*
 * Unlike the x86_64/aarch64 versions' GRAMINE_SYSCALL path... no: exactly like the aarch64
 * version, this issues a plain `ecall` rather than going through GRAMINE_SYSCALL. The macro
 * needs g_pal_tcb_singleton, which is only resolvable from within PAL/LibOS's own shared objects
 * (see its definition in pal_arch_exception.c) -- but gramine_call() is called from *guest
 * application* code (this header is also meant for patched libc/test programs to use), a
 * separate, unrelated binary that has no link relationship to libpal.so at all. A raw `ecall`
 * works correctly instead: it traps via the same seccomp filter as every other syscall under
 * the riscv64 design (no libc patching), lands in libos_syscall_entry via
 * maybe_emulate_syscall(), and libos_syscalls.c already checks the incoming syscall number
 * against GRAMINE_CUSTOM_SYSCALL_NR generically (architecture-independent) to route it to
 * handle_libos_call() -- so this needs no special-casing on the LibOS side at all, just the
 * right number in a7 here.
 *
 * a0/a1/a2 already hold number/arg1/arg2 exactly where handle_libos_call(args[0], args[1],
 * args[2]) expects them (ALL_SYSCALL_ARGS pulls straight from context->regs[10..15]), so, unlike
 * x86_64's version, no register shuffling is needed beyond setting a7 -- and because this ends
 * in a normal C `return` (not a bare asm block standing in for the whole function body), there's
 * no -Wreturn-type concern either. `ecall` clobbers no registers, so the "+r"(x0) output is
 * exact.
 */
static int gramine_call(int number, unsigned long arg1, unsigned long arg2) {
    register long a0 __asm__("a0") = number;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a7 __asm__("a7") = GRAMINE_CUSTOM_SYSCALL_NR;
    __asm__ volatile(
        "ecall\n"
        : "+r"(a0)
        : "r"(a1), "r"(a2), "r"(a7)
        : "memory"
    );
    return (int)a0;
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

static inline void gramine_rwlock_write_unlock(void* lock) {
    gramine_call(GRAMINE_CALL_RWLOCK_WRITE_UNLOCK, (unsigned long)lock, 0);
}

#endif /* __ASSEMBLER__ */

#endif /* GRAMINE_ENTRY_API_H_ */
