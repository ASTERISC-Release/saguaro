#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Build CPython for a VBS enclave image (phase 3e): with scripts/vbs/vbs-gcc against the vbs glibc
# sysroot, installed into DESTDIR (default <repo>/build-vbs-python). Optional modules whose
# libraries are not in the sysroot (zlib, OpenSSL, libffi, ...) are left out; with --sci (the python example on x64, into build-vbs-python-sci) the zlib and
# ctypes modules are built (zlib 1.3.1 and libffi 3.4.6 from source, linked statically): the SciPy wheels of
# the python example import both.
#
# Usage (aarch64 Linux, the build container or WSL):
#   scripts/vbs/build-python.sh [--if-needed] [--sci] [Python-X.Y.Z.tgz] [DESTDIR]
# Without a tarball, Python 3.12.7 is downloaded from python.org into ~/.cache/gramine-vbs/dist
# and checked against its SHA-256. With --if-needed nothing is done when DESTDIR already holds a
# build of the same tarball against the same sysroot (the stamp file DESTDIR/.vbs-python); the
# Python example's vbs.build uses it.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IF_NEEDED=
[ "$1" = --if-needed ] && { IF_NEEDED=1; shift; }
SCI=
[ "$1" = --sci ] && { SCI=1; shift; }
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
ZLIB_VERSION=1.3.1
FFI_VERSION=3.4.6
FFI_SHA256=b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
WITH_ZLIB=$SCI
DEST=$(realpath -m "${2:-$ROOT/build-vbs-python${SCI:+-sci}}")
SYSROOT=${SYSROOT:-$ROOT/build-vbs-glibc}
STAMP="$(sha256sum < "$TARBALL" | cut -c1-64) $(sha256sum < "$SYSROOT/usr/lib/libc.so.6" | cut -c1-64)${WITH_ZLIB:+ zlib-$ZLIB_VERSION libffi-$FFI_VERSION}"
if [ -n "$IF_NEEDED" ] && [ "$(cat "$DEST/.vbs-python" 2>/dev/null)" = "$STAMP" ]; then
    echo "python in $DEST is up to date"
    exit 0
fi
WORK="$DEST.build"
rm -rf "$WORK" "$DEST"
mkdir -p "$WORK"
tar -C "$WORK" -xzf "$TARBALL"
ZCONF=
if [ -n "$WITH_ZLIB" ]; then
    ZTAR=$HOME/.cache/gramine-vbs/dist/zlib-$ZLIB_VERSION.tar.gz
    if [ ! -f "$ZTAR" ]; then
        mkdir -p "$(dirname "$ZTAR")"
        echo "downloading zlib-$ZLIB_VERSION.tar.gz"
        curl -sSfL -o "$ZTAR.part" "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
        echo "$ZLIB_SHA256  $ZTAR.part" | sha256sum -c --quiet \
            || { rm -f "$ZTAR.part"; echo "zlib-$ZLIB_VERSION.tar.gz: SHA-256 mismatch"; exit 1; }
        mv "$ZTAR.part" "$ZTAR"
    fi
    tar -C "$WORK" -xzf "$ZTAR"
    (cd "$WORK"/zlib-$ZLIB_VERSION && CC="$ROOT/scripts/vbs/vbs-gcc" CFLAGS="-O2 -fPIC" \
        ./configure --static --prefix="$WORK/zlib" && make -j"$(nproc)" && make install) \
        >"$WORK/zlib.log" 2>&1 || { tail -20 "$WORK/zlib.log"; exit 1; }
    FTAR=$HOME/.cache/gramine-vbs/dist/libffi-$FFI_VERSION.tar.gz
    if [ ! -f "$FTAR" ]; then
        echo "downloading libffi-$FFI_VERSION.tar.gz"
        curl -sSfL -o "$FTAR.part" "https://github.com/libffi/libffi/releases/download/v$FFI_VERSION/libffi-$FFI_VERSION.tar.gz"
        echo "$FFI_SHA256  $FTAR.part" | sha256sum -c --quiet \
            || { rm -f "$FTAR.part"; echo "libffi-$FFI_VERSION.tar.gz: SHA-256 mismatch"; exit 1; }
        mv "$FTAR.part" "$FTAR"
    fi
    tar -C "$WORK" -xzf "$FTAR"
    (cd "$WORK"/libffi-$FFI_VERSION && CC="$ROOT/scripts/vbs/vbs-gcc" CFLAGS="-O2 -fPIC" \
        ./configure --disable-shared --enable-static --with-pic --disable-docs \
            --prefix="$WORK/ffi" && make -j"$(nproc)" && make install) \
        >"$WORK/ffi.log" 2>&1 || { tail -20 "$WORK/ffi.log"; exit 1; }
    ZCONF="LIBFFI_CFLAGS=-I$WORK/ffi/include LIBFFI_LIBS=\"-L$WORK/ffi/lib -lffi\" ZLIB_CFLAGS=-I$WORK/zlib/include ZLIB_LIBS=\"-L$WORK/zlib/lib -lz\""
fi
cd "$WORK"/Python-*
eval ./configure --prefix=/usr --without-ensurepip --disable-test-modules $ZCONF \
    CC=\"$ROOT/scripts/vbs/vbs-gcc\" >"$WORK/configure.log" 2>&1 \
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
