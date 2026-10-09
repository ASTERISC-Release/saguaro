#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# syntax-check-x86.sh — type-check the LibOS and Linux-PAL sources against the *x86_64* arch headers
# on an aarch64 host (no x86 toolchain needed). It reuses the compile commands of an existing
# aarch64 build directory, swaps include/arch/aarch64 -> include/arch/x86_64, defines __x86_64__
# and runs `gcc -fsyntax-only` (the assembler never runs, so x86 inline asm is not validated).
#
# This catches typos, missing declarations and signature mismatches in shared code and in the
# x86_64 arch headers after refactors made on aarch64. It is NOT a substitute for a real x86_64
# build and test run (see .github/workflows/multiarch.yml).
#
# Usage: scripts/syntax-check-x86.sh [build-dir]     (default: build-aarch64)
# Known, expected skips: aarch64-only sources, and libos_table.c (needs the x86 kernel's syscall
# numbers, which are absent from an aarch64 host's headers).
set -uo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
BUILD=${1:-$HERE/../build-aarch64}
STUB=$HERE/syntax-check-x86          # stub of the x86-only <asm/prctl.h>
[ -f "$BUILD/build.ninja" ] || { echo "no build.ninja in $BUILD (run install.sh first)"; exit 2; }
cd "$BUILD"

pass=0; fail=0; skipped=0
for obj in $(ninja -t targets all | grep -E '^(libos/src/libsysdb\.so\.p|pal/src/host/linux/libpal\.so\.p)/[^:]*\.c\.o:' | cut -d: -f1); do
    case "$obj" in
        *aarch64*|*libos_table.c.o) skipped=$((skipped+1)); continue ;;
    esac
    cmd=$(ninja -t commands "$obj" | tail -1 \
        | sed -E 's/ -MD -MQ [^ ]+ -MF [^ ]+//; s/ -o [^ ]+//; s/ -c / -fsyntax-only /; s#arch/aarch64#arch/x86_64#g')
    err=$(eval "$cmd -I$STUB -D__NR_arch_prctl=158 -U__aarch64__ -D__x86_64__ \
          -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion" 2>&1 >/dev/null \
          | sed -E 's/\x1b\[[0-9;]*[mK]//g' | grep -m3 -E 'error')
    if [ -z "$err" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $obj"; echo "$err" | sed 's/^/    /'; fi
done
echo "x86_64 syntax check: pass=$pass fail=$fail skipped=$skipped"
[ "$fail" -eq 0 ]
