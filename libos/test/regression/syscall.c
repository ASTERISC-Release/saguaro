#define _GNU_SOURCE
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>

int main(void) {
    long ret = 0;
#ifdef __x86_64__
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(__NR_getpid)
        : "memory", "cc", "rcx", "r11"
    );
#elif defined(__aarch64__)
    /* The aarch64 analog of the raw `syscall`: a bare `svc #0` with the number in x8. Under
     * Gramine's seccomp-based trap emulation this is exactly what an un-patched libc wrapper
     * executes, so the test exercises the whole trap-emulation entry path (the return into the
     * application clobbers x16/x17 and the link register by design, hence the clobber list;
     * x0 -- the result register -- survives as the output). */
    register long x8 __asm__("x8") = __NR_getpid;
    register long x0 __asm__("x0");
    __asm__ volatile (
        "svc #0"
        : "=r"(x0)
        : "r"(x8)
        : "memory", "x16", "x17", "x30"
    );
    ret = x0;
#elif defined(__riscv)
    /* The riscv64 analog of the raw `svc` above: a bare `ecall` with the number in a7. Under
     * Gramine's seccomp-based trap emulation this is exactly what an un-patched libc wrapper
     * executes, so the test exercises the whole trap-emulation entry path. Unlike aarch64,
     * `ecall` destroys *no* registers (riscv64 keeps the syscall number in a7, separate from
     * the a0 return value -- see pal_arch.h's pal_context_get_syscall comment -- and the trap
     * return path restores exactly), so no clobber list beyond `memory` is needed; the
     * exact-context-restore ebreak protocol (PAL_BRK_RESTORE_CONTEXT) restores app ra/sp by
     * construction, so they need no mention here either. */
    register long a7 __asm__("a7") = __NR_getpid;
    register long a0 __asm__("a0");
    __asm__ volatile (
        "ecall"
        : "=r"(a0)
        : "r"(a7)
        : "memory"
    );
    ret = a0;
#endif
    if (ret != 1)
        errx(EXIT_FAILURE, "getpid syscall: %ld (expected 1)", ret);

    puts("TEST OK");
    return 0;
}
