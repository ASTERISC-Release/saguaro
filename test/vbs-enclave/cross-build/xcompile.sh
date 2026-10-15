#!/bin/sh
# Gramine phase-0 spike (step g): compile a VBS enclave C source on Linux with clang for
# aarch64-pc-windows-msvc. The Windows SDK and MSVC headers are not redistributable; copy them from
# the Windows machine into a scratch folder first:
#   ssh <win> "tar -C 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0' -cf - um shared ucrt" | tar -C $HDR/sdk -xf -
#   ssh <win> "tar -C 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.50.35717' -cf - include" | tar -C $HDR/vc -xf -
# Usage: HDR=<scratch folder> xcompile.sh <source.c> <output.obj>
# The headers include each other with mixed case; missing names are linked to the existing file.
# Link, veiid and sign on Windows (see ../elf-in-image/link-clang-obj.cmd).
set -e
src=$1; out=$2
CL="clang --driver-mode=cl --target=aarch64-pc-windows-msvc -fms-compatibility-version=19.50 \
    /imsvc $HDR/vc/include /imsvc $HDR/sdk/ucrt /imsvc $HDR/sdk/um /imsvc $HDR/sdk/shared -Wno-everything"
for d in "$HDR/sdk" "$HDR/vc"; do
    find "$d" -type f | while read -r f; do
        l="$(dirname "$f")/$(basename "$f" | tr A-Z a-z)"
        [ "$f" != "$l" ] && [ ! -e "$l" ] && ln -s "$(basename "$f")" "$l"
    done
done
for i in $(seq 1 50); do
    miss=$($CL /c /O1 /guard:cf /MT -DUNICODE /Fo:"$out" "$src" 2>&1 | grep -oE "'[^']+' file not found" | head -1 | cut -d"'" -f2) || true
    [ -z "$miss" ] && break
    f=$(find "$HDR/sdk" "$HDR/vc" -iname "$(basename "$miss")" -not -type l | head -1)
    [ -z "$f" ] && { echo "header not found: $miss"; exit 1; }
    ln -sf "$(basename "$f")" "$(dirname "$f")/$(basename "$miss")"
done
$CL /c /O1 /guard:cf /MT -DUNICODE /Fo:"$out" "$src"
echo "compiled $out"
