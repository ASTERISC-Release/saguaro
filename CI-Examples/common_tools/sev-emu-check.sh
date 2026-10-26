#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# sev-emu-check.sh -- is QEMU_PATH a QEMU that can run the emulated SEV or TDX (SAGUARO_ACCEL=tcg)?
#
#   QEMU_PATH=<qemu-system-x86_64> sev-emu-check.sh sev|snp|tdx
#
# Checks that QEMU_PATH is set, is a QEMU, offers the TCG accelerator and has the object type of the mode
# (sev-emu for sev, sev-snp-emu for snp, tdx-emu for tdx): the dev-sev or dev-tdx branch of qemu-sev-tcg, not a
# stock QEMU. Prints one line on success and the reason on stderr (exit 1) otherwise. Used by exec.sh,
# test/sev/test-sev.sh and test/quick.sh.

mode=${1:-}
case $mode in
    sev) type=sev-emu; what="SEV"; branch=dev-sev ;;
    snp) type=sev-snp-emu; what="SEV"; branch=dev-sev ;;
    tdx) type=tdx-emu; what="TDX"; branch=dev-tdx ;;
    *) echo "usage: $0 sev|snp|tdx" >&2; exit 2 ;;
esac
fail() { echo "emulated $what: $*" >&2; exit 1; }

q=${QEMU_PATH:-}
[ -n "$q" ] || fail "QEMU_PATH is not set; with SAGUARO_ACCEL=tcg it must name the qemu-system-x86_64 of the $branch branch of qemu-sev-tcg"
[ -x "$q" ] || fail "QEMU_PATH=$q is not an executable"
version=$("$q" --version 2>/dev/null | sed -n '1s/^QEMU emulator version //p')
[ -n "$version" ] || fail "QEMU_PATH=$q is not a QEMU (--version printed no 'QEMU emulator version')"
"$q" -accel help 2>/dev/null | grep -qx 'tcg' || fail "QEMU $version ($q) has no TCG accelerator"
"$q" -object help 2>/dev/null | grep -qx "  $type" \
    || fail "QEMU $version ($q) has no '$type' object: it is not the $branch branch of qemu-sev-tcg (a stock QEMU emulates no $what)"
echo "emulated $what ($mode): QEMU $version, object $type, TCG; no attestation, no memory isolation"
