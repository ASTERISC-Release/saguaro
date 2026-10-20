#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Phase 1 of the Windows VBS-enclave PAL: compile the enclave and host translation units on LINUX
# with clang targeting aarch64-pc-windows-msvc, and assemble the AArch64 boundary thunk. This is
# the Linux half of the build (PLAN.md D3); the Windows half (link /ENCLAVE, veiid, signtool, and
# the host .exe) runs on the Windows machine -- see link-sign.cmd and build-host.cmd.
#
# The Windows SDK and MSVC headers are not redistributable; copy them from the Windows machine into
# a scratch folder and point HDR at it (same mechanism as test/vbs/arm64/cross-build/xcompile.sh):
#   HDR/sdk/{um,shared,ucrt}   from  <SDK>/Include/<ver>/{um,shared,ucrt}
#   HDR/vc/include             from  <VC>/Tools/MSVC/<ver>/include
#
# Packed objects (phase 2): PACK names the output folder of scripts/vbs/pack.py (packed.bin,
# pal_vbs_packed_blob.S, pal_vbs_packed_table.h); the in-enclave loader and the blob are then built
# too. Without PACK the image carries no packed objects and no loader (the phase-1 build).
#
# VBS_ARCH selects the target: aarch64 (ARM64 Windows) or x86_64 (x64 Windows,
# dev docs: vbs-enclave/PLAN-x64.md); the default is the machine's (uname -m), the
# architecture of the Linux objects built next to these.
#
# Usage: HDR=<scratch> [OUT=<dir>] [PACK=<pack.py output>] [VBS_ARCH=x86_64] scripts/vbs/build-objs.sh
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
VBS="$ROOT/pal/src/host/vbs"
OUT=${OUT:-"$ROOT/build-vbs/obj"}
: "${HDR:?set HDR to the folder holding the copied SDK (sdk/) and MSVC (vc/) headers}"
mkdir -p "$OUT"
case "${VBS_ARCH:-$(uname -m)}" in
    aarch64|arm64) TARGET=aarch64-pc-windows-msvc; BOUNDARY=pal_vbs_boundary.S ;;
    x86_64|x64|amd64) TARGET=x86_64-pc-windows-msvc; BOUNDARY=pal_vbs_boundary_x86_64.S ;;
    *) echo "VBS_ARCH must be aarch64 or x86_64"; exit 2 ;;
esac

# The compiler command lines below are built as strings: folders with spaces in their names are
# reached through links without spaces.
LINKS=$(mktemp -d "${TMPDIR:-/tmp}/build-objs.XXXXXX")
trap 'rm -rf "$LINKS"' EXIT
case "$VBS" in *" "*) ln -s "$VBS" "$LINKS/vbs"; VBS=$LINKS/vbs ;; esac
case "$HDR" in *" "*) ln -s "$(cd "$HDR" && pwd)" "$LINKS/hdr"; HDR=$LINKS/hdr ;; esac
INC="-I$VBS/common -I$VBS/enclave"
DEFS=""
if [ -n "$PACK" ]; then
    PACK=$(cd "$PACK" && pwd)
    case "$PACK" in *" "*) ln -s "$PACK" "$LINKS/pack"; PACK=$LINKS/pack ;; esac
    INC="$INC -I$PACK"
else
    DEFS="-DPAL_VBS_NO_PACKED"
fi
# /Brepro and -mno-incremental-linker-compatible: no time stamp in the COFF header (reproducible objects)
CL="clang --driver-mode=cl --target=$TARGET -fms-compatibility-version=19.50 /Brepro \
    /imsvc $HDR/vc/include /imsvc $HDR/sdk/ucrt /imsvc $HDR/sdk/um /imsvc $HDR/sdk/shared \
    -Wno-everything $INC"

# Case-insensitive header shims (the copied headers include each other with mixed case); made once
# per header folder.
for d in "$HDR/sdk" "$HDR/vc"; do
    [ -d "$d" ] || continue
    [ -f "$d/.shims-done" ] && continue
    find "$d" -type f | while read -r f; do
        l="$(dirname "$f")/$(basename "$f" | tr A-Z a-z)"
        if [ "$f" != "$l" ] && [ ! -e "$l" ]; then ln -s "$(basename "$f")" "$l"; fi
    done
    touch "$d/.shims-done"
done
resolve_missing() {
    for _ in $(seq 1 60); do
        miss=$($CL /c "$@" 2>&1 | grep -oE "'[^']+' file not found" | head -1 | cut -d"'" -f2) || true
        [ -z "$miss" ] && return 0
        f=$(find "$HDR/sdk" "$HDR/vc" -iname "$(basename "$miss")" -not -type l | head -1)
        [ -z "$f" ] && { echo "header not found: $miss"; return 1; }
        ln -sf "$(basename "$f")" "$(dirname "$f")/$(basename "$miss")"
    done
}

echo "=== enclave: pal_vbs_enclave.c"
resolve_missing /O1 /guard:cf /MT -DUNICODE $DEFS /Fo"$OUT/pal_vbs_enclave.obj" "$VBS/enclave/pal_vbs_enclave.c"
$CL /c /O1 /guard:cf /MT -DUNICODE $DEFS /Fo"$OUT/pal_vbs_enclave.obj" "$VBS/enclave/pal_vbs_enclave.c"

echo "=== enclave: pal_vbs_boot.c"
$CL /c /O1 /guard:cf /MT -DUNICODE $DEFS /Fo"$OUT/pal_vbs_boot.obj" "$VBS/enclave/pal_vbs_boot.c"

echo "=== enclave: pal_vbs_selftest.c"
$CL /c /O1 /guard:cf /MT -DUNICODE $DEFS /Fo"$OUT/pal_vbs_selftest.obj" "$VBS/test/pal_vbs_selftest.c"

if [ -n "$PACK" ]; then
    echo "=== enclave: pal_vbs_packed.c (loader of the packed objects)"
    $CL /c /O1 /guard:cf /MT -DUNICODE /W4 /Fo"$OUT/pal_vbs_packed.obj" "$VBS/enclave/pal_vbs_packed.c"
    echo "=== enclave: packed blob ($PACK/packed.bin)"
    (cd "$PACK" && clang --target=$TARGET -mno-incremental-linker-compatible -c pal_vbs_packed_blob.S \
        -o "$OUT/pal_vbs_packed_blob.obj")
fi

echo "=== enclave: $BOUNDARY (clang integrated assembler)"
clang --target=$TARGET -mno-incremental-linker-compatible -c "$VBS/enclave/$BOUNDARY" -o "$OUT/pal_vbs_boundary.obj"

echo "=== host: pal_vbs_host.c"
$CL /c /O2 /MT -DUNICODE -I"$VBS/common" /Fo"$OUT/pal_vbs_host.obj" "$VBS/host/pal_vbs_host.c"
$CL /c /O2 /MT -DUNICODE -I"$VBS/common" /Fo"$OUT/pal_vbs_host_io.obj" "$VBS/host/pal_vbs_host_io.c"
$CL /c /O2 /MT -DUNICODE -I"$VBS/common" /Fo"$OUT/pal_vbs_verify.obj" "$VBS/host/pal_vbs_verify.c"

echo "objects in $OUT:"
ls -l "$OUT"
