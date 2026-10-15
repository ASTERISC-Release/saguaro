#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Build the glibc that applications packed into a VBS enclave link against: glibc 2.39 (the
# version of the bundled glibc subproject, same tarball) compiled with -ffixed-x18, installed into
# a sysroot. x18 is the enclave TEB and the kernel resets it on preemption, so packed code must
# never use it (Documentation/devel/vbs-enclave/FINDINGS.md). No Gramine syscall patch is needed:
# pack.py rewrites the `svc #0` instructions at pack time.
#
# On x86_64 (Documentation/devel/vbs-enclave/PLAN-x64.md) no special compiler flag is needed: the
# same glibc is built as it is (no -ffixed-x18, no setcontext change, no soft-float routines), so
# that the applications, their libraries and the loader come from one known glibc in the same
# sysroot layout (/usr/lib) as on AArch64.
#
# Usage (aarch64 or x86_64 build container): scripts/vbs/build-glibc.sh [SYSROOT]   (default build-vbs-glibc)
# Then link a static PIE with scripts/vbs/cc-static-pie.sh.
#   scripts/vbs/build-glibc.sh --softfp [SYSROOT]   only (re)build SYSROOT/usr/lib/libsoftfp-x18.a
#                                                    (for a sysroot made before it existed)
set -e

SOFTFP_ONLY=
[ "$1" = --softfp ] && { SOFTFP_ONLY=1; shift; }
ARCH=$(uname -m)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SYSROOT=$(realpath -m "${1:-$ROOT/build-vbs-glibc}")
TARBALL="$ROOT/subprojects/packagecache/glibc-2.39.tar.gz"
WORK="$SYSROOT.build"

[ -f "$TARBALL" ] || { echo "missing $TARBALL (run a meson setup with -Dlibc=glibc once, or" \
                       "download it as subprojects/glibc-2.39-1.wrap says)"; exit 1; }

# The soft-float routines glibc takes from the compiler's libgcc.a were not built with -ffixed-x18
# (__divtf3 and __multf3 keep temporaries in x18; pack.py refuses them). They are built from glibc's
# own soft-fp sources with -ffixed-x18 (hidden, so every library and program keeps its private
# copy) into libsoftfp-x18.a, which is forced into the glibc links through a compiler wrapper and
# installed into the sysroot, where scripts/vbs/vbs-gcc and cc-*-pie.sh link it ahead of libgcc.
build_softfp() {  # source tree, output folder
    mkdir -p "$2"
    for f in divtf3 multf3; do
        gcc -O2 -ffixed-x18 -fPIC -fvisibility=hidden -include endian.h \
            -I "$1/soft-fp" -I "$1/sysdeps/aarch64" -I "$1/stdlib" -c "$1/soft-fp/$f.c" -o "$2/$f.o"
    done
    rm -f "$2/libsoftfp-x18.a"
    ar rcs "$2/libsoftfp-x18.a" "$2/divtf3.o" "$2/multf3.o"
}
if [ -n "$SOFTFP_ONLY" ] && [ "$ARCH" = x86_64 ]; then
    echo "x86_64: no soft-float routines are needed"
    exit 0
fi
if [ -n "$SOFTFP_ONLY" ]; then
    [ -f "$SYSROOT/usr/lib/libc.so.6" ] || { echo "no vbs glibc in $SYSROOT"; exit 1; }
    T=$(mktemp -d "${TMPDIR:-/tmp}/vbs-softfp.XXXXXX")
    tar -C "$T" -xzf "$TARBALL" glibc-2.39/soft-fp glibc-2.39/sysdeps/aarch64 glibc-2.39/stdlib
    build_softfp "$T/glibc-2.39" "$T/out"
    cp "$T/out/libsoftfp-x18.a" "$SYSROOT/usr/lib/"
    rm -rf "$T"
    echo "installed $SYSROOT/usr/lib/libsoftfp-x18.a"
    exit 0
fi
rm -rf "$WORK" "$SYSROOT"
mkdir -p "$WORK/build"
tar -C "$WORK" -xzf "$TARBALL"
patch --quiet -p1 --directory "$WORK/glibc-2.39" \
    < "$ROOT/subprojects/packagefiles/glibc-2.39/fix-fortify-source.patch"
if [ "$ARCH" = x86_64 ]; then
    cd "$WORK"
    mkdir -p build
    cd build
    printf 'slibdir=/usr/lib\nrtlddir=/usr/lib\n' > configparms
    ../glibc-2.39/configure --prefix=/usr --libdir=/usr/lib --with-tls --without-gd \
        --without-selinux --disable-sanity-checks --disable-test --disable-nscd --disable-werror \
        CC="gcc -Wl,-z,max-page-size=0x1000 -Wl,-z,common-page-size=0x1000" \
        CFLAGS="-O2 -Wno-unused-value" >"$WORK/configure.log" 2>&1
    make -j"$(nproc)" >"$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }
    make install DESTDIR="$SYSROOT" >"$WORK/install.log" 2>&1 \
        || { tail -30 "$WORK/install.log"; exit 1; }
    echo "glibc (x86_64) installed in $SYSROOT/usr"
    exit 0
fi
# setcontext() must not load x18 from the saved context: x18 is the TEB of the *current* thread
# (a context saved on one thread may be resumed on another). getcontext/swapcontext only store it.
sed -i 's/^\tldp\tx18, x19, \[x0, oX0 + 18 \* SZREG\]$/\tldr\tx19, [x0, oX0 + 19 * SZREG]/' \
    "$WORK/glibc-2.39/sysdeps/unix/sysv/linux/aarch64/setcontext.S"
grep -q 'ldr	x19, \[x0, oX0 + 19 \* SZREG\]' \
    "$WORK/glibc-2.39/sysdeps/unix/sysv/linux/aarch64/setcontext.S" \
    || { echo "setcontext.S: the x18 load was not found (glibc version?)"; exit 1; }
# the x18-free soft-float routines (see build_softfp above), forced into every glibc link
SOFTFP="$WORK/softfp-x18"
build_softfp "$WORK/glibc-2.39" "$SOFTFP"
cat > "$SOFTFP/cc" <<WRAP
#!/bin/sh
# compiler for the vbs glibc: link commands get the x18-free soft-float routines first
for a in "\$@"; do
    case "\$a" in -c|-S|-E|-M|-MM|-r) exec gcc -Wl,-z,max-page-size=0x1000 -Wl,-z,common-page-size=0x1000 "\$@" ;; esac
done
exec gcc -Wl,-z,max-page-size=0x1000 -Wl,-z,common-page-size=0x1000 \\
    -Wl,-u,__divtf3 -Wl,-u,__multf3 $SOFTFP/libsoftfp-x18.a "\$@"
WRAP
chmod +x "$SOFTFP/cc"

cd "$WORK/build"
# everything in /usr/lib (shared libraries and ld.so too): the paths the packed files get, and the
# dynamic loader's built-in search path
printf 'slibdir=/usr/lib\nrtlddir=/usr/lib\n' > configparms
# Shared objects with a 4 KiB maximum page size: ld.so maps an object whose p_align exceeds the page
# size through an aligned anonymous reservation and a MAP_FIXED file mapping inside it, an address
# that cannot be the object's packed address; with p_align = 4 KiB it lets the LibOS place the
# first mapping, which the vbs PAL turns into the packed address (strict W^X still holds per 4 KiB
# page, which pack.py checks).
../glibc-2.39/configure --prefix=/usr --libdir=/usr/lib --with-tls --without-gd --without-selinux \
    --disable-sanity-checks --disable-test --disable-nscd --disable-werror \
    CC="$SOFTFP/cc" \
    CFLAGS="-O2 -ffixed-x18 -Wno-unused-value" >"$WORK/configure.log" 2>&1
make -j"$(nproc)" >"$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }
make install DESTDIR="$SYSROOT" >"$WORK/install.log" 2>&1 || { tail -30 "$WORK/install.log"; exit 1; }
cp "$SOFTFP/libsoftfp-x18.a" "$SYSROOT/usr/lib/"
echo "glibc with -ffixed-x18 installed in $SYSROOT/usr"
