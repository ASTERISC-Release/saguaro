#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Build the glibc that applications packed into a VBS enclave link against: glibc 2.39 (the
# version of the bundled glibc subproject, same tarball) compiled with -ffixed-x18, installed into
# a sysroot. x18 is the enclave TEB and the kernel resets it on preemption, so packed code must
# never use it (Documentation/devel/vbs-enclave/FINDINGS.md). No Gramine syscall patch is needed:
# pack.py rewrites the `svc #0` instructions at pack time.
#
# Usage (aarch64 build container): scripts/vbs/build-glibc.sh [SYSROOT]   (default build-vbs-glibc)
# Then link a static PIE with scripts/vbs/cc-static-pie.sh.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SYSROOT=$(realpath -m "${1:-$ROOT/build-vbs-glibc}")
TARBALL="$ROOT/subprojects/packagecache/glibc-2.39.tar.gz"
WORK="$SYSROOT.build"

[ -f "$TARBALL" ] || { echo "missing $TARBALL (run a meson setup with -Dlibc=glibc once, or" \
                       "download it as subprojects/glibc-2.39-1.wrap says)"; exit 1; }
rm -rf "$WORK" "$SYSROOT"
mkdir -p "$WORK/build"
tar -C "$WORK" -xzf "$TARBALL"
patch --quiet -p1 --directory "$WORK/glibc-2.39" \
    < "$ROOT/subprojects/packagefiles/glibc-2.39/fix-fortify-source.patch"
cd "$WORK/build"
../glibc-2.39/configure --prefix=/usr --libdir=/usr/lib --with-tls --without-gd --without-selinux \
    --disable-sanity-checks --disable-test --disable-nscd --disable-werror \
    CFLAGS="-O2 -ffixed-x18 -Wno-unused-value" >"$WORK/configure.log" 2>&1
make -j"$(nproc)" >"$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }
make install DESTDIR="$SYSROOT" >"$WORK/install.log" 2>&1 || { tail -30 "$WORK/install.log"; exit 1; }
echo "glibc with -ffixed-x18 installed in $SYSROOT/usr"
