/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Minimal bring-up application for the aarch64 vm-PAL (tools/run-vm-aarch64-dev.sh).
 * Linked -static-pie: ET_DYN so LibOS mmaps it at an ASLR'd address inside DRAM (a non-PIE
 * 0x400680 link address has no physical backing on arm-virt), and no dynamic loader so the
 * B3 handoff exercises only the PAL/LibOS syscall paths. */
#include <stdio.h>

int main(void) {
    printf("Hello world from Gramine VM-PAL on aarch64!\n");
    fflush(stdout);
    return 0;
}
