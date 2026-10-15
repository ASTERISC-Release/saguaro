#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Build CPython for a VBS enclave image (phase 3e): with scripts/vbs/vbs-gcc against the vbs glibc
# sysroot, installed into DESTDIR (default <repo>/build-vbs-python). Optional modules whose
# libraries are not in the sysroot (zlib, OpenSSL, libffi, ...) are left out.
#
# Usage (aarch64 Linux, the build container or WSL):
#   scripts/vbs/build-python.sh [--if-needed] [Python-X.Y.Z.tgz] [DESTDIR]
# Without a tarball, Python 3.12.7 is downloaded from python.org into ~/.cache/gramine-vbs/dist
# and checked against its SHA-256. With --if-needed nothing is done when DESTDIR already holds a
# build of the same tarball against the same sysroot (the stamp file DESTDIR/.vbs-python); the
# Python example's vbs.build uses it.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IF_NEEDED=
[ "$1" = --if-needed ] && { IF_NEEDED=1; shift; }
VERSION=3.12.7
SHA256=73ac8fe780227bf371add8373c3079f42a0dc62deff8d612cd15a618082ab623
TARBALL=$1
if [ -z "$TARBALL" ]; then
    TARBALL=$HOME/.cache/gramine-vbs/dist/Python-$VERSION.tgz
    if [ ! -f "$TARBALL" ]; then
        mkdir -p "$(dirname "$TARBALL")"
        echo "downloading Python-$VERSION.tgz from python.org"
        curl -sSfL -o "$TARBALL.part" "https://www.python.org/ftp/python/$VERSION/Python-$VERSION.tgz"
        echo "$SHA256  $TARBALL.part" | sha256sum -c --quiet \
            || { rm -f "$TARBALL.part"; echo "Python-$VERSION.tgz: SHA-256 mismatch"; exit 1; }
        mv "$TARBALL.part" "$TARBALL"
    fi
fi
DEST=$(realpath -m "${2:-$ROOT/build-vbs-python}")
SYSROOT=${SYSROOT:-$ROOT/build-vbs-glibc}
STAMP="$(sha256sum < "$TARBALL" | cut -c1-64) $(sha256sum < "$SYSROOT/usr/lib/libc.so.6" | cut -c1-64)"
if [ -n "$IF_NEEDED" ] && [ "$(cat "$DEST/.vbs-python" 2>/dev/null)" = "$STAMP" ]; then
    echo "python in $DEST is up to date"
    exit 0
fi
WORK="$DEST.build"
rm -rf "$WORK" "$DEST"
mkdir -p "$WORK"
tar -C "$WORK" -xzf "$TARBALL"
cd "$WORK"/Python-*
./configure --prefix=/usr --without-ensurepip --disable-test-modules \
    CC="$ROOT/scripts/vbs/vbs-gcc" >"$WORK/configure.log" 2>&1 \
    || { tail -20 "$WORK/configure.log"; exit 1; }
make -j"$(nproc)" >"$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }
make install DESTDIR="$DEST" >"$WORK/install.log" 2>&1 || { tail -20 "$WORK/install.log"; exit 1; }
grep -A40 "The necessary bits to build these optional modules were not found" "$WORK/make.log" \
    | head -20 || true
rm -rf "$WORK"
# no debug information in the packed copy (the interpreter and the extension modules)
find "$DEST/usr" -type f \( -name '*.so' -o -name 'python3.*' \) -exec sh -c \
    'file -b "$1" | grep -q ELF && strip --strip-debug "$1"' sh {} \; 2>/dev/null || true
echo "$STAMP" > "$DEST/.vbs-python"
echo "python installed in $DEST/usr"
