#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# sev-emu-check.sh -- is QEMU_PATH a QEMU that can run the emulated SEV or TDX (SAGUARO_ACCEL=tcg)?
#
#   QEMU_PATH=<qemu-system-x86_64> sev-emu-check.sh sev|snp|tdx
#
# Checks that QEMU_PATH is set, is a QEMU, offers the TCG accelerator and has the object type of the mode
# (sev-emu for sev, sev-snp-emu for snp, tdx-emu for tdx): the QEMU of vmm/sev-tcg or vmm/tdx-tcg (built with
# scripts/qemu.sh build sev-tcg|tdx-tcg), not a stock QEMU. Prints one line on success and the reason on stderr
# (exit 1) otherwise. Used by exec.sh, test/sev/test-sev.sh and test/quick.sh.

mode=${1:-}
case $mode in
    sev) type=sev-emu; what="SEV"; vmm=sev-tcg ;;
    snp) type=sev-snp-emu; what="SEV"; vmm=sev-tcg ;;
    tdx) type=tdx-emu; what="TDX"; vmm=tdx-tcg ;;
    *) echo "usage: $0 sev|snp|tdx" >&2; exit 2 ;;
esac
fail() { echo "emulated $what: $*" >&2; exit 1; }
build="build it with 'scripts/qemu.sh build $vmm' and set QEMU_PATH=\$(scripts/qemu.sh path $vmm)"

q=${QEMU_PATH:-}
[ -n "$q" ] || fail "QEMU_PATH is not set; with SAGUARO_ACCEL=tcg it must name the QEMU of vmm/$vmm: $build"
[ -x "$q" ] || fail "QEMU_PATH=$q is not an executable; $build"
version=$("$q" --version 2>/dev/null | sed -n '1s/^QEMU emulator version //p')
[ -n "$version" ] || fail "QEMU_PATH=$q is not a QEMU (--version printed no 'QEMU emulator version')"
"$q" -accel help 2>/dev/null | grep -qx 'tcg' || fail "QEMU $version ($q) has no TCG accelerator"
"$q" -object help 2>/dev/null | grep -qx "  $type" \
    || fail "QEMU $version ($q) has no '$type' object: it is not the QEMU of vmm/$vmm (a stock QEMU emulates no $what); $build"
echo "emulated $what ($mode): QEMU $version, object $type, TCG; no attestation, no memory isolation"
