#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Build the packed test shared object with the GNU AArch64 Linux toolchain (native aarch64 gcc, for
# example in WSL Ubuntu on the ARM64 Windows machine, as phase 0 step h did; or any aarch64-linux-gnu
# cross gcc). Output: libpacked.so next to this script (or in $1).
#
#   -fPIC -shared     a real shared object with dynamic relocations (what the LibOS/glibc are)
#   -ffixed-x18       x18 is the enclave TEB on Windows ARM64 (PLAN.md D4)
#   -z now            no lazy PLT binding: the in-enclave loader resolves every JUMP_SLOT eagerly
#   -z norelro        keep it simple for the first experiment (RELRO re-protection is a later step)
#   -nostdlib         freestanding: no crt, no libc
set -e
cd "$(dirname "$0")"
OUT=${1:-.}
CC=${CC:-gcc}
$CC -O2 -fPIC -shared -nostdlib -ffixed-x18 -fno-stack-protector -fno-asynchronous-unwind-tables \
    -fvisibility=default -Wl,-z,now -Wl,-z,norelro -Wl,--hash-style=sysv -Wl,-soname,libpacked.so \
    -o "$OUT/libpacked.so" packed_test.c
readelf -h -l -d "$OUT/libpacked.so" | grep -E 'Machine|Type:|LOAD|DYNAMIC|RELA|JMPREL|FLAGS|BIND_NOW'
readelf -r "$OUT/libpacked.so" | grep -E 'R_AARCH64' | awk '{print $3}' | sort | uniq -c
echo "built $OUT/libpacked.so"
