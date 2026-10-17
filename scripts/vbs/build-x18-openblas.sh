#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Rebuild OpenBLAS 0.3.26 (the version of Ubuntu 24.04; it includes the reference LAPACK and
# CBLAS/LAPACKE) so that no instruction names x18 (the enclave TEB on Windows ARM64; pack.py refuses
# it; Documentation/devel/vbs-enclave/X18-SCAN.md). C and Fortran code get -ffixed-x18. The AArch64
# kernels written in assembly ignore that flag, so they are patched first:
#  - `stp x18, x19, [...]` / `ldp x18, x19, [...]` (the prologue and epilogue of 48 kernels) become
#    `stp xzr, x19, ...` / `ldp xzr, x19, ...` (same stack layout, x18 untouched);
#  - a kernel that uses x18 as a working register has x18/w18 replaced by a caller-saved register
#    (x16 down to x9) that the file does not mention anywhere; if none is free the script stops
#    and names the file.
# Needs the compiler and gfortran built by scripts/vbs/build-x18-runtime.sh (run it first).
#
# Everything is under PREFIX (default ~/.local/share/gramine-vbs/x18-runtime): sources, build,
# the result in PREFIX/openblas-lib. No root; network for the source tarball only.
# Usage: scripts/vbs/build-x18-openblas.sh [--test] [PREFIX]    (--test: also run OpenBLAS's tests)
set -eu

TEST=
[ "${1:-}" = --test ] && { TEST=1; shift; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PREFIX=$(realpath -m "${1:-$HOME/.local/share/gramine-vbs/x18-runtime}")
[ "$(uname -m)" = aarch64 ] || { echo "aarch64 only" >&2; exit 1; }
case "$PREFIX" in "$HOME"/*) ;; *) echo "PREFIX must be under \$HOME" >&2; exit 1;; esac
GCCP=$PREFIX/gcc
[ -x "$GCCP/bin/gfortran" ] || { echo "run build-x18-runtime.sh first (no $GCCP/bin/gfortran)" >&2; exit 1; }

VERSION=0.3.26
SHA256=4e6e4f5cb14c209262e33e6816d70221a2fe49eb69eaf0a06f065598ac602c68
URL=https://github.com/OpenMathLib/OpenBLAS/releases/download/v$VERSION/OpenBLAS-$VERSION.tar.gz
SRC=$PREFIX/src; BLD=$PREFIX/build/openblas; OUT=$PREFIX/openblas-lib
mkdir -p "$SRC" "$BLD" "$OUT"
log() { echo "[x18-openblas] $*"; }

[ -f "$SRC/OpenBLAS-$VERSION.tar.gz" ] || "$ROOT/CI-Examples/common_tools/download" \
    --output "$SRC/OpenBLAS-$VERSION.tar.gz" --sha256 "$SHA256" --url "$URL"
echo "$SHA256  $SRC/OpenBLAS-$VERSION.tar.gz" | sha256sum -c - >/dev/null
rm -rf "$BLD" && mkdir -p "$BLD" && tar -C "$BLD" -xzf "$SRC/OpenBLAS-$VERSION.tar.gz"
cd "$BLD/OpenBLAS-$VERSION"

python3 - <<'PY'
import glob, re, sys
bad = []
for f in sorted(glob.glob('kernel/arm64/*.S')):
    t = open(f).read()
    if not re.search(r'\b[xw]18\b', t):
        continue
    t = re.sub(r'\b(stp|ldp)(\s+)x18,(\s*)x19,', r'\1\2xzr,\3x19,', t)
    if re.search(r'\b[xw]18\b', t):
        used = set(re.findall(r'\b[xw](\d+)\b', t))
        for n in (16, 17, 15, 14, 13, 12, 11, 10, 9):
            if str(n) not in used:
                t = re.sub(r'\bx18\b', f'x{n}', t)
                t = re.sub(r'\bw18\b', f'w{n}', t)
                print(f'{f}: x18 -> x{n}')
                break
        else:
            bad.append(f)
    open(f, 'w').write(t)
if bad:
    sys.exit('no free caller-saved register in: ' + ' '.join(bad))
PY

export LD_LIBRARY_PATH="$GCCP/lib64:$PREFIX/deps/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
s=$(date +%s)
make -j"$(nproc)" DYNAMIC_ARCH=1 USE_OPENMP=0 USE_THREAD=1 NUM_THREADS=8 NO_AFFINITY=1 \
    BUILD_LAPACK_DEPRECATED=0 NO_STATIC=1 CC=gcc FC="$GCCP/bin/gfortran" \
    COMMON_OPT="-O2 -ffixed-x18" FCOMMON_OPT="-O2 -ffixed-x18" >"$BLD/make.log" 2>&1 \
    || { tail -30 "$BLD/make.log"; exit 1; }
log "make took $(( $(date +%s) - s )) s"
make PREFIX="$BLD/inst" NO_STATIC=1 install >"$BLD/install.log" 2>&1 || { tail -20 "$BLD/install.log"; exit 1; }
rm -rf "$OUT" && mkdir -p "$OUT" && cp -P "$BLD"/inst/lib/libopenblas* "$OUT/" 2>/dev/null || true
if [ -n "$TEST" ]; then
    s=$(date +%s)
    make DYNAMIC_ARCH=1 USE_OPENMP=0 USE_THREAD=1 NUM_THREADS=8 NO_AFFINITY=1 NO_STATIC=1 \
        CC=gcc FC="$GCCP/bin/gfortran" tests >"$BLD/tests.log" 2>&1 \
        && log "OpenBLAS tests passed in $(( $(date +%s) - s )) s" \
        || { tail -30 "$BLD/tests.log"; log "OpenBLAS tests FAILED"; exit 1; }
fi

log "scan (zero x18 instructions required)"
rm -rf "$BLD/scan" && mkdir -p "$BLD/scan" && cp -L "$OUT"/libopenblas*.so* "$BLD/scan/"
python3 "$ROOT/scripts/vbs/syscall-gaps/x18-scan.py" "$PREFIX/x18-openblas-scan.tsv" "$BLD/scan" \
    | tee "$PREFIX/x18-openblas-scan.log"
awk -F'\t' 'NR>1 && $4>0 {bad=1} END {exit bad}' "$PREFIX/x18-openblas-scan.tsv" \
    && log "OK: no library names x18" || { log "FAILED: see $PREFIX/x18-openblas-scan.tsv"; exit 1; }
