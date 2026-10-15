#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Compile and link a dynamically linked PIE against the vbs glibc sysroot (build-glibc.sh), with
# -ffixed-x18 and a 64 KiB max page size. Its interpreter is the sysroot's /usr/lib/ld-linux-aarch64
# .so.1, which together with /usr/lib/libc.so.6 must be packed into the image at those paths
# (pack.py --obj user:...). Kernel UAPI headers come from the build system.
#
# Usage: SYSROOT=build-vbs-glibc scripts/vbs/cc-dynamic-pie.sh -o OUT SOURCES...
set -e
: "${SYSROOT:?set SYSROOT to the vbs glibc sysroot}"
CC=${CC:-gcc}
LIB="$SYSROOT/usr/lib"
# the x18-free soft-float routines ahead of libgcc's (build-glibc.sh)
SOFTFP=
[ -f "$LIB/libsoftfp-x18.a" ] && SOFTFP=$LIB/libsoftfp-x18.a
exec $CC -O2 -ffixed-x18 -fPIE -pie -nostdinc -nostartfiles -nodefaultlibs \
    -isystem "$($CC -print-file-name=include)" -isystem "$SYSROOT/usr/include" \
    -idirafter /usr/include/$($CC -dumpmachine) -idirafter /usr/include \
    -Wl,-z,max-page-size=0x10000 -Wl,--dynamic-linker=/usr/lib/ld-linux-aarch64.so.1 \
    "$LIB/Scrt1.o" "$LIB/crti.o" "$($CC -print-file-name=crtbeginS.o)" \
    "$@" \
    $SOFTFP "$LIB/libc.so.6" "$LIB/libc_nonshared.a" "$LIB/ld-linux-aarch64.so.1" -lgcc \
    "$($CC -print-file-name=crtendS.o)" "$LIB/crtn.o"
