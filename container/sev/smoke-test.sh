#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# smoke-test.sh: boot a stock Linux guest (OVMF + kernel + the tiny initrd of
# make-smoke-initrd.py) as a plain, SEV, SEV-ES or SEV-SNP guest, to check the confidential-VM
# launch path of the host, QEMU and firmware without any Gramine code (dev docs: sev/).
#
# Usage: container/sev/smoke-test.sh none|sev|sev-es|snp
#
# Environment (defaults in brackets):
#   QEMU_PATH   QEMU binary [qemu-system-x86_64 from PATH]; snp needs QEMU >= 9.1
#   FIRMWARE    OVMF image, used with -bios [/root/sev-tools/firmware/edk2-2025.11/OVMF.amdsev.fd]
#   KERNEL      guest kernel [/root/sev-tools/guest/vmlinuz]
#   INITRD      guest initrd [/root/sev-tools/guest/smoke-initrd.cpio]
#   SMP, MEM    vCPUs and memory [1, 2G]; CPU model [host]
#   CBITPOS     C-bit position, CPUID 0x8000001F EBX[5:0] [51, the value of the EPYC 7443P host]
#   HASHES      kernel-hashes=on|off [on]
#   TIMEOUT     seconds before the guest is killed [120]
#   EXTRA       extra QEMU arguments
set -uo pipefail

MODE=${1:-}
QEMU=${QEMU_PATH:-$(command -v qemu-system-x86_64)}
FIRMWARE=${FIRMWARE:-/root/sev-tools/firmware/edk2-2025.11/OVMF.amdsev.fd}
KERNEL=${KERNEL:-/root/sev-tools/guest/vmlinuz}
INITRD=${INITRD:-/root/sev-tools/guest/smoke-initrd.cpio}
SMP=${SMP:-1}
MEM=${MEM:-2G}
CPU=${CPU:-host}
CBITPOS=${CBITPOS:-51}
HASHES=${HASHES:-on}
TIMEOUT=${TIMEOUT:-120}

case $MODE in
    none)   CGS=() ;;
    # policy bit 0: NODBG; bit 2: ES. reduced-phys-bits=1 is what the AMD documents use on Milan.
    sev)    CGS=(-machine confidential-guest-support=sev0
                 -object "sev-guest,id=sev0,cbitpos=$CBITPOS,reduced-phys-bits=1,policy=0x1,kernel-hashes=$HASHES") ;;
    sev-es) CGS=(-machine confidential-guest-support=sev0
                 -object "sev-guest,id=sev0,cbitpos=$CBITPOS,reduced-phys-bits=1,policy=0x5,kernel-hashes=$HASHES") ;;
    # SNP policy default 0x30000 (bit 17 reserved-must-be-one, bit 16 SMT allowed).
    snp)    CGS=(-machine confidential-guest-support=sev0
                 -object "sev-snp-guest,id=sev0,cbitpos=$CBITPOS,reduced-phys-bits=1,kernel-hashes=$HASHES") ;;
    *) echo "usage: $0 none|sev|sev-es|snp" >&2; exit 2 ;;
esac

echo "QEMU: $("$QEMU" --version | head -n 1) ($QEMU)"
echo "firmware: $FIRMWARE"
echo "mode: $MODE, smp $SMP, mem $MEM, cpu $CPU"
START=$(date +%s.%N)
timeout --foreground -k 5 "$TIMEOUT" "$QEMU" -enable-kvm -machine q35 -cpu "$CPU" -smp "$SMP" -m "$MEM" \
    "${CGS[@]}" -bios "$FIRMWARE" \
    -kernel "$KERNEL" -initrd "$INITRD" -append "console=ttyS0 panic=-1 quiet loglevel=4" \
    -nographic -no-reboot -net none -monitor none -serial stdio ${EXTRA:-} 2>&1 \
    | tr -d '\r' | grep -a -v -E '^\s*$'
RC=${PIPESTATUS[0]}
END=$(date +%s.%N)
echo "exit status $RC, $(awk -v a="$START" -v b="$END" 'BEGIN{printf "%.2f", b-a}') s"
exit "$RC"
