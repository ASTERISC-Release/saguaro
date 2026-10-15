#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Compile and link a static PIE against the vbs glibc sysroot (scripts/vbs/build-glibc.sh), with
# -ffixed-x18 and a 64 KiB max page size (strict W^X per page in the packed image). Kernel UAPI
# headers (linux/, asm/) come from the build system, after the sysroot's own headers.
#
# Usage: SYSROOT=build-vbs-glibc scripts/vbs/cc-static-pie.sh -o OUT SOURCES... [-lpthread ...]
set -e
: "${SYSROOT:?set SYSROOT to the vbs glibc sysroot}"
CC=${CC:-gcc}
LIB="$SYSROOT/usr/lib"
# the x18-free soft-float routines ahead of libgcc's (build-glibc.sh)
SOFTFP=
[ -f "$LIB/libsoftfp-x18.a" ] && SOFTFP=$LIB/libsoftfp-x18.a
# -ffixed-x18 on AArch64 only (x18 is the enclave TEB there; x86_64 needs no flag, PLAN-x64.md)
X18=-ffixed-x18
[ "$(uname -m)" = x86_64 ] && X18=
exec $CC -O2 $X18 -fPIE -static-pie -nostdinc -nostartfiles -nodefaultlibs \
    -isystem "$($CC -print-file-name=include)" -isystem "$SYSROOT/usr/include" \
    -idirafter /usr/include/$($CC -dumpmachine) -idirafter /usr/include \
    -Wl,-z,max-page-size=0x10000 \
    "$LIB/rcrt1.o" "$LIB/crti.o" "$($CC -print-file-name=crtbeginS.o)" \
    "$@" \
    $SOFTFP -L"$LIB" -Wl,--start-group -lc -lgcc -lgcc_eh -Wl,--end-group \
    "$($CC -print-file-name=crtendS.o)" "$LIB/crtn.o"
