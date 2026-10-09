/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Minimal bring-up application for the aarch64 vm-PAL (tools/run-vm-aarch64-dev.sh).
 * Linked -static-pie: ET_DYN so LibOS mmaps it at an ASLR'd address inside DRAM (a non-PIE
 * 0x400680 link address has no physical backing on arm-virt), and no dynamic loader so the
 * B3 handoff exercises only the PAL/LibOS syscall paths. */
#include <stdio.h>
#include <time.h>

int main(void) {
    printf("Hello world from Gramine VM-PAL on aarch64!\n");
    fflush(stdout);

    /* B3/B4 acceptance: spend most of the run executing in EL0 so the 100 ms periodic
     * timer tick arrives while the *user* thread is running -- the el0_irq preemption
     * path (save_userland_context -> scheduler -> isr_eret_to_userland) that x86 proves
     * with its own suite. clock_gettime in the loop re-crosses the svc/eret boundary
     * between spins, so a register-clobber regression corrupts the loop, not just a
     * one-shot print. */
    volatile unsigned long spins = 0;
    for (int i = 0; i < 4; i++) {
        struct timespec ts = {0};
        unsigned long target = spins + 40000000UL; /* ~1 tick under TCG */
        while (spins < target) {
            spins++;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        printf("spin %d done, ts.tv_sec=%ld tv_nsec=%ld\n", i, ts.tv_sec, ts.tv_nsec);
        fflush(stdout);
    }
    printf("EL0 preemption soak complete\n");
    fflush(stdout);
    return 0;
}
