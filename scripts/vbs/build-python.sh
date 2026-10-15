#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Build CPython for a VBS enclave image (phase 3e): with scripts/vbs/vbs-gcc against the vbs glibc
# sysroot, installed into DESTDIR (default <repo>/build-vbs-python). Optional modules whose
# libraries are not in the sysroot (zlib, OpenSSL, libffi, ...) are left out.
#
# Usage (aarch64 build container): scripts/vbs/build-python.sh [Python-X.Y.Z.tgz] [DESTDIR]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TARBALL=${1:-/tmp/Python-3.12.7.tgz}
DEST=$(realpath -m "${2:-$ROOT/build-vbs-python}")
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
echo "python installed in $DEST/usr"
