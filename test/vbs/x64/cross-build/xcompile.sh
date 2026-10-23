#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

# Saguaro x64 phase-0 spike (step g): compile a VBS enclave C source on Linux with clang for
# x86_64-pc-windows-msvc. The Windows SDK and MSVC headers are not redistributable; copy them from
# the Windows machine into a scratch folder first:
#   ssh <win> "tar -C 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0' -cf - um shared ucrt" | tar -C $HDR/sdk -xf -
#   ssh <win> "tar -C '<VS folder>/VC/Tools/MSVC/14.50.35717' -cf - include" | tar -C $HDR/vc -xf -
# Usage: HDR=<scratch folder> [XFLAGS="/I dir ..."] xcompile.sh <source.c> <output.obj>
# The headers include each other with mixed case; missing names are linked to the existing file.
# Link, veiid and sign on Windows (see link-clang-obj.cmd). The x64 copy of
# test/vbs/arm64/cross-build/xcompile.sh: only the target differs.
set -e
src=$1; out=$2
CL="clang --driver-mode=cl --target=x86_64-pc-windows-msvc -fms-compatibility-version=19.50 \
    /imsvc $HDR/vc/include /imsvc $HDR/sdk/ucrt /imsvc $HDR/sdk/um /imsvc $HDR/sdk/shared -Wno-everything $XFLAGS"
for d in "$HDR/sdk" "$HDR/vc"; do
    find "$d" -type f | while read -r f; do
        l="$(dirname "$f")/$(basename "$f" | tr A-Z a-z)"
        if [ "$f" != "$l" ] && [ ! -e "$l" ]; then ln -s "$(basename "$f")" "$l"; fi
    done
done
for i in $(seq 1 50); do
    miss=$($CL /c /O1 /guard:cf /MT -DUNICODE /Fo:"$out" "$src" 2>&1 | grep -oE "'[^']+' file not found" | head -1 | cut -d"'" -f2) || true
    if [ -z "$miss" ]; then break; fi
    f=$(find "$HDR/sdk" "$HDR/vc" -iname "$(basename "$miss")" -not -type l | head -1)
    if [ -z "$f" ]; then echo "header not found: $miss"; exit 1; fi
    ln -sf "$(basename "$f")" "$(dirname "$f")/$(basename "$miss")"
done
$CL /c /O1 /guard:cf /MT -DUNICODE /Fo:"$out" "$src"
echo "compiled $out"
