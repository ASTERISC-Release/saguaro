#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# The WSL half of the Windows-only build (dev docs: vbs-enclave/PLAN-windows-build.md):
# started from Windows by gramine-vbs.ps1 or build-enclave.ps1 with its inputs in environment
# variables (WSLENV; the /p ones arrive as Linux paths), never on a command line.
#
#   wsl-build.sh sync      unpack the tar of a Windows folder into the distribution
#       GV_TAR   tar file (translated); GV_DEST  folder in the distribution
#       Files with CRLF line ends (a checkout made with core.autocrlf=true) get LF line ends and
#       files starting with #! the executable bit (NTFS keeps none); modification times are kept,
#       so unchanged files do not rebuild. Files of the previous sync that are no longer in the
#       tar are removed (the list is in GV_DEST/.gramine-vbs-synced).
#   wsl-build.sh objs      compile the Windows enclave runtime and host (build-objs.sh)
#       GV_PACK  pack.py output folder in the distribution (empty: no packed objects)
#   wsl-build.sh manifest  the whole Linux side for a manifest: the vbs glibc and the meson build of
#                          the runtime when missing (build-runtime.sh), then `gramine-vbs build --no-sign` (the
#                          manifest's vbs.build recipe, pack.py, build-objs.sh)
#       GV_MANIFEST  the manifest in the distribution
#   objs and manifest also take
#       GV_OUT       Windows output folder (translated): gets the objects, the .def file,
#                    link-sign.cmd (CRLF), objs-info.txt (versions, hashes) and, for a manifest,
#                    host-dirs.txt (the folders vbs.allowed_files needs below the host root)
#       GV_HDRID     the SDK and MSVC versions, e.g. 10.0.26100.0-14.50.35717
#       GV_HDRTAR    tar (translated) of the SDK's um, shared, ucrt and MSVC's include folders; only
#                    needed when the headers of GV_HDRID are not cached yet (exit code 10 asks)
#
# The headers are unpacked once per GV_HDRID into ~/.cache/gramine-vbs/winhdr/<id> (the
# case-insensitive name shims of build-objs.sh need a writable copy in a case-sensitive file
# system; a tar made by tar.exe on Windows and unpacked here takes 4 s, `cp -r` from /mnt/c 26 s).
# Exit codes: 10 headers needed, 11 tools missing (wsl-setup.sh), others: a failed step.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOLS=${GRAMINE_VBS_TOOLS:-$HOME/.local/share/gramine-vbs/tools}
[ -f "$TOOLS/env.sh" ] && . "$TOOLS/env.sh"
MODE=${1:-}

log() { echo "[wsl] $*"; }

if [ "$MODE" = sync ]; then
    : "${GV_TAR:?}" "${GV_DEST:?}"
    mkdir -p "$GV_DEST"
    tar -C "$GV_DEST" -xf "$GV_TAR"
    python3 -I - "$GV_DEST" "$GV_TAR" <<'PY'
import os, sys, tarfile
dest, tar = sys.argv[1], sys.argv[2]
n = 0
names = set()
with tarfile.open(tar) as t:
    for m in t:
        if not m.isfile():
            continue
        names.add(os.path.normpath(m.name))
        p = os.path.join(dest, m.name)
        with open(p, 'rb') as f:
            data = f.read()
        st = os.stat(p)
        if data.startswith(b'#!') and not st.st_mode & 0o100:
            os.chmod(p, st.st_mode | 0o111)   # tar.exe on NTFS keeps no executable bits
        if b'\r\n' not in data or b'\0' in data[:8000]:
            continue
        with open(p, 'wb') as f:
            f.write(data.replace(b'\r\n', b'\n'))
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
        n += 1
if n:
    print(f'[wsl] {n} files had Windows (CRLF) line ends: converted to LF')
# files of the previous sync that are gone on Windows are removed here too (build outputs, which
# were never synced, stay)
lst = os.path.join(dest, '.gramine-vbs-synced')
try:
    old = set(open(lst).read().splitlines())
except OSError:
    old = set()
gone = sorted(old - names)
for name in gone:
    try:
        os.remove(os.path.join(dest, name))
    except OSError:
        pass
if gone:
    print(f'[wsl] {len(gone)} files deleted on Windows: removed from the copy')
with open(lst, 'w') as f:
    f.write('\n'.join(sorted(names)) + '\n')
PY
    exit 0
fi

case "$MODE" in objs|manifest) ;; *) echo "usage: wsl-build.sh sync|objs|manifest"; exit 2 ;; esac
: "${GV_OUT:?}" "${GV_HDRID:?}"
sh "$HERE/wsl-setup.sh" check clang >/dev/null || { sh "$HERE/wsl-setup.sh" check clang; exit 11; }

HDR=${GRAMINE_VBS_HEADERS:-$HOME/.cache/gramine-vbs/winhdr/$GV_HDRID}
if [ ! -f "$HDR/.complete" ]; then
    [ -n "$GV_HDRTAR" ] && [ -f "$GV_HDRTAR" ] || exit 10
    log "unpacking the Windows SDK and MSVC headers ($GV_HDRID) into $HDR"
    rm -rf "$HDR"
    mkdir -p "$HDR/sdk" "$HDR/vc"
    tar -C "$HDR/sdk" -xf "$GV_HDRTAR" um shared ucrt
    tar -C "$HDR/vc" -xf "$GV_HDRTAR" include
    touch "$HDR/.complete"
fi
mkdir -p "$GV_OUT"

if [ "$MODE" = objs ]; then
    OBJ=$(mktemp -d "${TMPDIR:-/tmp}/gramine-vbs-obj.XXXXXX")
    trap 'rm -rf "$OBJ" "$OBJ.log"' EXIT
    HDR="$HDR" OUT="$OBJ" PACK="$GV_PACK" sh "$HERE/build-objs.sh" >"$OBJ.log" 2>&1 \
        || { tail -30 "$OBJ.log"; exit 1; }
else
    : "${GV_MANIFEST:?}"
    GRAMINE_VBS_LOG_PREFIX=[wsl] sh "$HERE/build-runtime.sh"
    python3 -I "$HERE/gramine-vbs" build "$GV_MANIFEST" --no-sign --runtime "$ROOT/build-vbs" \
        --headers "$HDR"
    B=${GV_MANIFEST%.toml}
    B=${B%.vbs}
    OBJ=${B%.manifest}.vbs/obj
    python3 -I - "$GV_MANIFEST" > "$GV_OUT/host-dirs.txt" <<'PY'
import sys, tomllib
m = tomllib.load(open(sys.argv[1], 'rb'))
for p in m.get('vbs', {}).get('allowed_files', []):
    if isinstance(p, str) and p.startswith('/') and p.endswith('/'):
        print(p.strip('/').replace('/', '\\'))
PY
fi

rm -f "$GV_OUT"/*.obj
cp "$OBJ"/*.obj "$GV_OUT/"
cp "$ROOT/pal/src/host/vbs/enclave/pal_vbs_enclave.def" "$GV_OUT/"
sed 's/\r*$/\r/' "$HERE/link-sign.cmd" > "$GV_OUT/link-sign.cmd"
{
    echo "clang: $(clang --version | head -1)"
    echo "gcc: $(gcc -dumpfullversion)"
    echo "headers: $GV_HDRID"
    echo "source: $(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo 'not a git checkout')"
    [ -z "$GV_MANIFEST" ] || echo "manifest: $GV_MANIFEST"
    (cd "$OBJ" && sha256sum *.obj)
} > "$GV_OUT/objs-info.txt"
log "objects: $(ls "$OBJ" | grep -c '\.obj$') -> $GV_OUT"
