#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# The WSL half of build-enclave.ps1 (Windows-only build): compile the Windows enclave runtime and
# host with clang (build-objs.sh) inside the WSL distribution and copy the objects, the .def file
# and link-sign.cmd (CRLF) into a Windows folder. Started by build-enclave.ps1 with its inputs in
# the environment (WSLENV; /p variables arrive as Linux paths):
#
#   GV_PACK     pack.py output folder (Linux path); empty: an image without packed objects
#   GV_OUT      Windows output folder (translated to its /mnt/... path)
#   GV_HDRID    a name of the header versions, e.g. 10.0.26100.0-14.50.35717
#   GV_HDRTAR   a tar file (translated) of the SDK's um, shared, ucrt and MSVC's include folders;
#               only needed when the headers of GV_HDRID are not cached yet (exit code 10 asks
#               for it)
#
# The SDK and MSVC headers are unpacked once per GV_HDRID into ~/.cache/gramine-vbs/winhdr/<id> (the
# case-insensitive name shims of build-objs.sh need a writable copy in a case-sensitive file
# system; a tar made by tar.exe on Windows and unpacked here takes 4 s, `cp -r` from /mnt/c 26 s).
# Toolchain versions and object hashes are written to <GV_OUT>/objs-info.txt.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOLS=${GRAMINE_VBS_TOOLS:-$HOME/.local/share/gramine-vbs/tools}
[ -f "$TOOLS/env.sh" ] && . "$TOOLS/env.sh"
command -v clang >/dev/null || { echo "no clang in WSL: scripts/vbs/wsl-setup.sh check clang"; exit 2; }
: "${GV_OUT:?}" "${GV_HDRID:?}"

HDR=${GRAMINE_VBS_HEADERS:-$HOME/.cache/gramine-vbs/winhdr/$GV_HDRID}
if [ ! -f "$HDR/.complete" ]; then
    [ -n "$GV_HDRTAR" ] && [ -f "$GV_HDRTAR" ] || exit 10
    echo "unpacking the Windows SDK and MSVC headers ($GV_HDRID) into $HDR"
    rm -rf "$HDR"
    mkdir -p "$HDR/sdk" "$HDR/vc"
    tar -C "$HDR/sdk" -xf "$GV_HDRTAR" um shared ucrt
    tar -C "$HDR/vc" -xf "$GV_HDRTAR" include
    touch "$HDR/.complete"
fi

OBJ=$(mktemp -d "${TMPDIR:-/tmp}/gramine-vbs-obj.XXXXXX")
trap 'rm -rf "$OBJ"' EXIT
if [ -n "$GV_PACK" ]; then
    HDR="$HDR" OUT="$OBJ" PACK="$GV_PACK" sh "$HERE/build-objs.sh" >"$OBJ.log" 2>&1 \
        || { tail -30 "$OBJ.log"; rm -f "$OBJ.log"; exit 1; }
else
    HDR="$HDR" OUT="$OBJ" sh "$HERE/build-objs.sh" >"$OBJ.log" 2>&1 \
        || { tail -30 "$OBJ.log"; rm -f "$OBJ.log"; exit 1; }
fi
rm -f "$OBJ.log"

mkdir -p "$GV_OUT"
rm -f "$GV_OUT"/*.obj
cp "$OBJ"/*.obj "$GV_OUT/"
cp "$ROOT/pal/src/host/vbs/enclave/pal_vbs_enclave.def" "$GV_OUT/"
sed 's/\r*$/\r/' "$HERE/link-sign.cmd" > "$GV_OUT/link-sign.cmd"
{
    echo "clang: $(clang --version | head -1)"
    echo "headers: $GV_HDRID"
    echo "source: $(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)"
    (cd "$OBJ" && sha256sum *.obj)
} > "$GV_OUT/objs-info.txt"
echo "objects: $(ls "$OBJ" | wc -l) in $GV_OUT"
