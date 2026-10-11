/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Phase 3 verification for the riscv64 port (Documentation/devel/riscv/plan.md): what the
 * kernel reports on a SIGTRAP from `ebreak`, which pal_arch_handle_trap() (the exact-context-
 * restore protocol, aarch64 finding #15's counterpart) builds its discrimination on:
 *
 *   - is the saved pc at the ebreak instruction or 4 bytes past it?
 *   - is the 4-byte word at the saved pc the ebreak encoding (what the handler reads)?
 *   - what is si_addr for this SIGTRAP?
 *   - does the *compressed* c.ebreak (0x9002, what a compiler may emit) trap identically?
 *   - where does the kernel point `ra` on signal delivery (the other Phase-1 "still to
 *     verify" item): the vDSO __kernel_rt_sigreturn, or something else?
 *
 * riscv64-only (reads riscv sigcontext fields). Static-link and run in the guest:
 *   riscv64-linux-gnu-gcc -static -O2 -o ebreak_trap ebreak_trap.c
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static volatile int g_in_handler;
static uintptr_t g_saved_pc[2];
static uintptr_t g_si_addr[2];
static uintptr_t g_saved_ra[2];
static unsigned g_nr;

/* glibc's riscv64 mcontext_t is { unsigned long __gregs[32]; union fpregs }, with REG_PC=0
 * and REG_RA=1, laid out over the kernel's struct sigcontext (sc_regs is struct
 * user_regs_struct: pc,ra,sp,gp,tp,t0,t1,t2,s0,s1,a0..a7,s2..s11,t3..t6 -- so gregs[0] is
 * pc and gregs[1] is ra). */
static void handler(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = ucv;
    uintptr_t* gregs = uc->uc_mcontext.__gregs;
    unsigned n = g_nr;
    if (n < 2) {
        g_saved_pc[n] = gregs[REG_PC];
        g_si_addr[n] = (uintptr_t)si->si_addr;
        g_saved_ra[n] = gregs[REG_RA];
        g_nr++;
    }
    g_in_handler++;

    /* Resume control: if the kernel resumes *at* the trapping instruction (aarch64-style),
     * a plain return would loop here forever; advance pc by the instruction size so the
     * second delivery happens exactly once per ebreak. If the kernel already advanced pc,
     * the same store is a harmless skip of the next (4-byte) instruction... so instead only
     * rewind-forwards when the word at pc is itself an ebreak, which is exactly the
     * ambiguity we are measuring: */
    uintptr_t pc = gregs[REG_PC];
    uint32_t insn_at_pc = *(uint32_t*)pc;
    uint16_t insn16 = *(uint16_t*)pc;
    if (insn_at_pc == 0x00000073) {          /* 4-byte ebreak at pc */
        gregs[REG_PC] = pc + 4;
    } else if (insn16 == 0x9002) {           /* compressed c.ebreak at pc */
        gregs[REG_PC] = pc + 2;
    }
    __sync_synchronize();
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGTRAP, &sa, NULL)) {
        perror("sigaction");
        return 1;
    }

    uintptr_t ebreak_addr;
    /* first: full-width ebreak (anchored by a linker symbol: `la` of a numeric local label
     * across an intervening instruction is exactly the hand-rolled pcrel pattern that GNU ld
     * rejects -- Phase 1 log) */
    __asm__ volatile(".global riscv_test_ebreak\n"
                     "riscv_test_ebreak:\n"
                     "ebreak\n");
    extern char riscv_test_ebreak[];
    ebreak_addr = (uintptr_t)riscv_test_ebreak;
    printf("ebreak instruction address : 0x%lx\n", (unsigned long)ebreak_addr);
    printf("handler #1 saved pc        : 0x%lx (delta %+ld)\n",
           (unsigned long)g_saved_pc[0],
           (long)((unsigned long)g_saved_pc[0] - (unsigned long)ebreak_addr));
    printf("handler #1 si_addr         : 0x%lx\n", (unsigned long)g_si_addr[0]);
    printf("handler #1 saved ra        : 0x%lx\n", (unsigned long)g_saved_ra[0]);

    /* where does that `ra` point? find the vDSO mapping and test membership */
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    unsigned long vdso_lo = 0, vdso_hi = 0;
    while (f && fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        char path[256];
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %255s", &lo, &hi, path) == 3 &&
                !strcmp(path, "[vdso]")) {
            vdso_lo = lo;
            vdso_hi = hi;
        }
    }
    if (f)
        fclose(f);
    printf("vdso                       : [%lx, %lx)\n", vdso_lo, vdso_hi);
    printf("saved ra in vdso           : %s\n",
           (vdso_lo && g_saved_ra[0] >= vdso_lo && g_saved_ra[0] < vdso_hi) ? "YES" : "no");

    uintptr_t cebreak_addr;
    /* second: compressed c.ebreak (2 bytes), same anchoring scheme */
    __asm__ volatile(".global riscv_test_cebreak\n"
                     "riscv_test_cebreak:\n"
                     ".short 0x9002\n");
    extern char riscv_test_cebreak[];
    cebreak_addr = (uintptr_t)riscv_test_cebreak;
    /* The handler above already advanced pc past whichever encoding sat at pc, so the
     * program has continued after each trap; handler #2's raw pc tells us where the kernel
     * put it on entry for the 2-byte instruction. */
    printf("c.ebreak instruction addr  : 0x%lx\n", (unsigned long)cebreak_addr);
    printf("handler #2 saved pc        : 0x%lx (delta %+ld)\n",
           (unsigned long)g_saved_pc[1],
           (long)((unsigned long)g_saved_pc[1] - (unsigned long)cebreak_addr));

    /* verdict */
    printf("verdict: pc on handler entry is %s ebreak (width-4 delta %+ld)\n",
           (g_saved_pc[0] == ebreak_addr) ? "AT the"
           : (g_saved_pc[0] == ebreak_addr + 4) ? "PAST the" : "WEIRD-distance from the",
           (long)(g_saved_pc[0] - ebreak_addr));
    puts("PASS");
    return 0;
}
