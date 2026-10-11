#!/bin/sh

set -e

log() {
    echo "glibc: $*"
}

CPU_FAMILY="$1"
CURRENT_SOURCE_DIR="$2"
VENDOR_SOURCE_DIR="$3"
CURRENT_BUILD_DIR="$4"
PRIVATE_DIR="$5"
PREFIX="$6"
LIBDIR="$7"
EXTRA_CFLAGS="$8"
shift 8

# Defaults for native builds; a cross build (see the `env:` of the `glibc` custom target in
# packagefiles/glibc-2.39/meson.build) overrides all of these: the custom target's command
# always runs on the build machine, so without this the riscv64 cross-build would silently
# build an x86_64 glibc with the host gcc. `${VAR:=default}` (rather than plain `VAR=default`)
# because the environment may already carry them; empty CONFIGURE_HOST means "native".
: "${CC:=gcc}" "${CXX:=g++}" "${AS:=$CC}" "${CONFIGURE_HOST:=}"
CFLAGS="-O2 -Wno-unused-value $EXTRA_CFLAGS"
CPPFLAGS="\
    -I$(realpath "$CURRENT_SOURCE_DIR")/../../libos/include \
    -I$(realpath "$CURRENT_SOURCE_DIR")/../../libos/include/arch/$CPU_FAMILY \
"
export CC CXX AS CFLAGS CPPFLAGS

BUILD_LOG=$(realpath "$CURRENT_BUILD_DIR/glibc-build.log")
rm -f "$BUILD_LOG"

log "see $BUILD_LOG for full build log"

log "preparing sources..."

rm -rf "$PRIVATE_DIR"
cp -ar "$VENDOR_SOURCE_DIR" "$PRIVATE_DIR"

for patch in "$CURRENT_SOURCE_DIR"/*.patch
do
    patch --quiet -p1 --directory "$PRIVATE_DIR" <"$patch"
done

BUILDDIR="$PRIVATE_DIR"/build

mkdir -p "$BUILDDIR"

(
    cd "$BUILDDIR"

    log "running configure..."
    ../configure \
        --prefix="$PREFIX" \
        --libdir="$PREFIX"/"$LIBDIR"/gramine/runtime/glibc \
        ${CONFIGURE_HOST:+--host="$CONFIGURE_HOST"} \
        --with-tls \
        --without-gd \
        --without-selinux \
        --disable-sanity-checks \
        --disable-test \
        --disable-nscd \
        >>"$BUILD_LOG" 2>&1

    log "running make..."
    make -j"$(nproc)" >>"$BUILD_LOG" 2>&1
)

for output in "$@"
do
    cp -aP "$BUILDDIR/$output" "$CURRENT_BUILD_DIR"/
done

log "done"
