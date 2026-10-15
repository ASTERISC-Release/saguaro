#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Compile and link a static PIE against the vbs glibc sysroot (scripts/vbs/build-glibc.sh), with
# -ffixed-x18 and a 64 KiB max page size (strict W^X per page in the packed image).
#
# Usage: SYSROOT=build-vbs-glibc scripts/vbs/cc-static-pie.sh -o OUT SOURCES... [-lpthread ...]
set -e
: "${SYSROOT:?set SYSROOT to the vbs glibc sysroot}"
CC=${CC:-gcc}
LIB="$SYSROOT/usr/lib"
exec $CC -O2 -ffixed-x18 -fPIE -static-pie -nostdinc -nostartfiles -nodefaultlibs \
    -isystem "$($CC -print-file-name=include)" -isystem "$SYSROOT/usr/include" \
    -Wl,-z,max-page-size=0x10000 \
    "$LIB/rcrt1.o" "$LIB/crti.o" "$($CC -print-file-name=crtbeginS.o)" \
    "$@" \
    -L"$LIB" -Wl,--start-group -lc -lgcc -lgcc_eh -Wl,--end-group \
    "$($CC -print-file-name=crtendS.o)" "$LIB/crtn.o"
