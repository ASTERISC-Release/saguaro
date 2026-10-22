#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Rebuild the runtime libraries that applications packed into a VBS enclave link against, with
# -ffixed-x18, as build-glibc.sh does for glibc: libgcc_s.so.1, libstdc++.so.6, libgfortran.so.5
# (from GCC) and libz.so.1 (zlib). x18 is the enclave TEB on Windows ARM64, so packed code must
# never use it (pack.py refuses it; dev docs: vbs-enclave/X18-SCAN.md).
#
# Everything goes under PREFIX (default ~/.local/share/saguaro-vbs/x18-runtime): the sources, the
# build trees, the GCC that builds the libraries and the result in PREFIX/lib. Nothing is installed
# system-wide and no root is needed. Network: the two source tarballs (checked by SHA-256 below) and
# the Ubuntu gmp/mpfr/mpc packages (apt-get download, no root; the headers GCC needs to build).
#
# GCC 13.3.0 is the compiler of Ubuntu 24.04 (the host gcc), so the host compiler builds the same
# C++ ABI the system libstdc++ has (libstdc++.so.6.0.32; the Ubuntu 24.04 package is 6.0.33).
# The libraries are built for the host's glibc 2.39 headers, which are the vbs glibc's version.
#
# Usage: scripts/vbs/build-x18-runtime.sh [--zlib-only] [PREFIX]
# Result: PREFIX/lib/{libz.so.1,libgcc_s.so.1,libstdc++.so.6,libgfortran.so.5}, then it scans them
# (scripts/vbs/syscall-gaps/x18-scan.py) and fails if any names x18.
set -eu

ZLIB_ONLY=
[ "${1:-}" = --zlib-only ] && { ZLIB_ONLY=1; shift; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PREFIX=$(realpath -m "${1:-$HOME/.local/share/saguaro-vbs/x18-runtime}")
[ "$(uname -m)" = aarch64 ] || { echo "aarch64 only" >&2; exit 1; }
case "$PREFIX" in "$HOME"/*) ;; *) echo "PREFIX must be under \$HOME" >&2; exit 1;; esac

GCC_VERSION=13.3.0
GCC_SHA256=0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083
GCC_URL=https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz
ZLIB_VERSION=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
ZLIB_URL=https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz

X18="-ffixed-x18"
SRC=$PREFIX/src; BLD=$PREFIX/build; DEPS=$PREFIX/deps; LIB=$PREFIX/lib
mkdir -p "$SRC" "$BLD" "$DEPS" "$LIB"
log() { echo "[x18-runtime] $*"; }
timed() { s=$(date +%s); "$@"; log "$1 took $(( $(date +%s) - s )) s"; }

fetch() {  # file url sha256
    [ -f "$SRC/$1" ] || "$ROOT/CI-Examples/common_tools/download" --output "$SRC/$1" \
        --sha256 "$3" --url "$2"
    echo "$3  $SRC/$1" | sha256sum -c - >/dev/null
}

build_zlib() {
    fetch zlib-$ZLIB_VERSION.tar.gz $ZLIB_URL $ZLIB_SHA256
    rm -rf "$BLD/zlib" && mkdir -p "$BLD/zlib" && tar -C "$BLD/zlib" -xzf "$SRC/zlib-$ZLIB_VERSION.tar.gz"
    cd "$BLD/zlib/zlib-$ZLIB_VERSION"
    CFLAGS="-O2 -fPIC $X18" ./configure --prefix="$PREFIX/zlib" >"$BLD/zlib.configure.log" 2>&1
    make -j"$(nproc)" >"$BLD/zlib.make.log" 2>&1 || { tail -20 "$BLD/zlib.make.log"; exit 1; }
    make install >"$BLD/zlib.install.log" 2>&1
    cp -P "$PREFIX"/zlib/lib/libz.so* "$LIB/"
}

build_gcc() {
    fetch gcc-$GCC_VERSION.tar.xz $GCC_URL $GCC_SHA256
    # gmp, mpfr, mpc: development packages unpacked in DEPS (not installed)
    if [ ! -f "$DEPS/usr/include/aarch64-linux-gnu/gmp.h" ]; then
        (cd "$DEPS" && for p in libgmp10 libgmp-dev libmpfr6 libmpfr-dev libmpc3 libmpc-dev; do
            apt-get download "$p" >/dev/null 2>&1 </dev/null || { echo "apt-get download $p failed" >&2; exit 1; }
        done; for f in *.deb; do dpkg-deb -x "$f" "$DEPS"; done)
    fi
    D=$DEPS/usr
    export LD_LIBRARY_PATH="$D/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    rm -rf "$BLD/gcc" && mkdir -p "$BLD/gcc/build" && tar -C "$BLD/gcc" -xJf "$SRC/gcc-$GCC_VERSION.tar.xz"
    cd "$BLD/gcc/build"
    # Only the runtime libraries matter; the compiler that builds them is a normal one. The target
    # libraries get -ffixed-x18 through the *_FOR_TARGET flags (libgcc, libstdc++-v3, libgfortran).
    ../gcc-$GCC_VERSION/configure --prefix="$PREFIX/gcc" \
        --enable-languages=c,c++,fortran --disable-bootstrap --disable-multilib --disable-nls \
        --disable-libsanitizer --disable-libgomp --disable-libquadmath --disable-libatomic \
        --disable-libssp --disable-libvtv --disable-libitm --disable-libstdcxx-pch \
        --with-gmp-include="$D/include/aarch64-linux-gnu" --with-gmp-lib="$D/lib/aarch64-linux-gnu" \
        --with-mpfr-include="$D/include" --with-mpfr-lib="$D/lib/aarch64-linux-gnu" \
        --with-mpc-include="$D/include" --with-mpc-lib="$D/lib/aarch64-linux-gnu" \
        CFLAGS_FOR_TARGET="-O2 -g1 $X18" CXXFLAGS_FOR_TARGET="-O2 -g1 $X18" \
        FCFLAGS_FOR_TARGET="-O2 -g1 $X18" >"$BLD/gcc.configure.log" 2>&1 \
        || { tail -30 "$BLD/gcc.configure.log"; exit 1; }
    make -j"$(nproc)" >"$BLD/gcc.make.log" 2>&1 || { tail -40 "$BLD/gcc.make.log"; exit 1; }
    make install >"$BLD/gcc.install.log" 2>&1 || { tail -20 "$BLD/gcc.install.log"; exit 1; }
    for l in libgcc_s.so.1 libstdc++.so.6 libgfortran.so.5; do
        f=$(ls "$PREFIX"/gcc/lib64/$l* 2>/dev/null | head -1)
        [ -n "$f" ] || f=$(ls "$PREFIX"/gcc/lib/$l* 2>/dev/null | head -1)
        [ -n "$f" ] || { echo "missing $l" >&2; exit 1; }
        cp -L "$f" "$LIB/$l"
    done
}

timed build_zlib
[ -n "$ZLIB_ONLY" ] || timed build_gcc

log "scan (zero x18 instructions required)"
rm -rf "$BLD/scan" && mkdir -p "$BLD/scan" && cp -L "$LIB"/lib*.so* "$BLD/scan/"
python3 "$ROOT/scripts/vbs/syscall-gaps/x18-scan.py" "$PREFIX/x18-scan.tsv" "$BLD/scan" | tee "$PREFIX/x18-scan.log"
awk -F'\t' 'NR>1 && $4>0 {bad=1} END {exit bad}' "$PREFIX/x18-scan.tsv" \
    && log "OK: no library names x18" || { log "FAILED: see $PREFIX/x18-scan.tsv"; exit 1; }
