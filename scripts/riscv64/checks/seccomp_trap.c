/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Phase 0 check of the riscv64 port (dev docs: riscv/PLAN.md): the mechanism the direct
 * PAL uses to intercept system calls. A seccomp filter traps one system call (getppid); the SIGSYS
 * handler inspects what the kernel reports, emulates the call by writing a result into the saved
 * register state, and returns. Builds for x86_64, aarch64 and riscv64 so the three can be compared.
 *
 *   riscv64-linux-gnu-gcc -static -O2 -o seccomp_trap seccomp_trap.c
 */

#define _GNU_SOURCE
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define EMULATED_RESULT 4242

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1 /* si_code of a SIGSYS raised by a seccomp filter */
#endif

#if defined(__x86_64__)
#define ARCH_NAME       "x86_64"
#define SYSCALL_INSN_SZ 2
#define UC_PC(uc)       ((uc)->uc_mcontext.gregs[REG_RIP])
#define UC_RET(uc)      ((uc)->uc_mcontext.gregs[REG_RAX])
#define UC_NR(uc)       ((uc)->uc_mcontext.gregs[REG_RAX])
#elif defined(__aarch64__)
#define ARCH_NAME       "aarch64"
#define SYSCALL_INSN_SZ 4
#define UC_PC(uc)       ((uc)->uc_mcontext.pc)
#define UC_RET(uc)      ((uc)->uc_mcontext.regs[0])
#define UC_NR(uc)       ((uc)->uc_mcontext.regs[8])
#elif defined(__riscv) && __riscv_xlen == 64
#define ARCH_NAME       "riscv64"
#define SYSCALL_INSN_SZ 4
#define UC_PC(uc)       ((uc)->uc_mcontext.__gregs[REG_PC])
#define UC_RET(uc)      ((uc)->uc_mcontext.__gregs[REG_A0])
#define UC_NR(uc)       ((uc)->uc_mcontext.__gregs[REG_A0 + 7]) /* a7 */
#else
#error "unsupported architecture"
#endif

static volatile int g_traps;
static siginfo_t g_info;
static uintptr_t g_pc;
static unsigned long g_nr_reg;
static uint32_t g_insn;

static void sigsys_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    ucontext_t* uc = ctx;
    g_info   = *info;
    g_pc     = (uintptr_t)UC_PC(uc);
    g_nr_reg = (unsigned long)UC_NR(uc);
    g_insn   = 0;
    memcpy(&g_insn, (void*)(g_pc - SYSCALL_INSN_SZ), SYSCALL_INSN_SZ);
    UC_RET(uc) = EMULATED_RESULT;
    g_traps++;
}

int main(void) {
    struct sigaction sa = {.sa_sigaction = sigsys_handler, .sa_flags = SA_SIGINFO};
    if (sigaction(SIGSYS, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {.len = sizeof(filter) / sizeof(filter[0]), .filter = filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ||
            syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) < 0) {
        perror("seccomp");
        return 1;
    }

    long ret = syscall(__NR_getppid);

    printf("arch                 %s\n", ARCH_NAME);
    printf("traps                %d\n", g_traps);
    printf("syscall() returned   %ld (expected %d)\n", ret, EMULATED_RESULT);
    printf("si_code              %d (SYS_SECCOMP is %d)\n", g_info.si_code, SYS_SECCOMP);
    printf("si_syscall           %d (__NR_getppid is %d)\n", g_info.si_syscall, (int)__NR_getppid);
    printf("si_arch              0x%x\n", g_info.si_arch);
    printf("si_call_addr         %p\n", g_info.si_call_addr);
    printf("saved pc             %p (%s si_call_addr)\n", (void*)g_pc,
           (void*)g_pc == g_info.si_call_addr ? "equals" : "DIFFERS FROM");
    printf("syscall-number reg   %lu\n", g_nr_reg);
    printf("insn before pc       0x%0*x (%d bytes)\n", SYSCALL_INSN_SZ * 2, g_insn, SYSCALL_INSN_SZ);

    int ok = g_traps == 1 && ret == EMULATED_RESULT && g_info.si_code == SYS_SECCOMP &&
             g_info.si_syscall == __NR_getppid;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
