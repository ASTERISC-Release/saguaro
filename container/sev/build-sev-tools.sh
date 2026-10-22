#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# build-sev-tools.sh: build the AMD SEV tooling of dev docs: sev/ inside the x86_64
# development container (container/run.sh), without root and without changing the image:
#
#   qemu    QEMU with `sev-snp-guest` and IGVM support (stock Ubuntu 24.04 QEMU 8.2 has only
#           `sev-guest`), linked against libigvm (built first, step `igvm`)
#   tools   snpguest, snphost, sevctl (Rust, cargo) and sev-snp-measure (Python, venv)
#   firmware  OVMF (edk2 2025.11) from Ubuntu 26.04's ovmf-amdsev/ovmf-generic packages into
#           firmware/edk2-2025.11/: Ubuntu 24.04's edk2 2024.02 does not boot SEV-SNP guests here
#   guest   the stock-Linux smoke-test guest (Ubuntu 24.04 kernel, busybox, sev-guest/tsm modules)
#           in guest/, for container/sev/smoke-test.sh (uses apt-get inside the container)
#
# Usage (inside the container; `all` is the default):
#   container/sev/build-sev-tools.sh [igvm|qemu|tools|firmware|guest|all]
#
# Everything goes to $SEV_TOOLS_DIR (default /root/sev-tools): src/ (downloads), build/, and the
# install prefix prefix/ (prefix/bin/qemu-system-x86_64, prefix/bin/snpguest, ...). container/run.sh
# mounts a directory `sev-tools/` that sits next to the checkout (or $SEV_TOOLS_DIR on the host) at
# /root/sev-tools, so the build survives container recreation; create it before the container is
# created (`mkdir ../sev-tools`). Select the QEMU with
#   export QEMU_PATH=/root/sev-tools/prefix/bin/qemu-system-x86_64
# (the saguaro-vm convention for a QEMU outside the image) and the tools with
#   export PATH=/root/sev-tools/prefix/bin:$PATH
#
# Sources are pinned. The QEMU tarball is verified with its upstream OpenPGP signature against the
# release key vendored next to this script (fingerprint below) and with the SHA-256 recorded when
# the version was pinned; the GitHub tarballs (igvm, the Rust tools) have no upstream signature
# and are checked against SHA-256 values recorded on first download (trust on first use, see
# dev docs: sev/FINDINGS.md). Rust dependencies come from crates.io through each
# project's Cargo.lock (`--locked`), which cargo checks by checksum.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
TOP=${SEV_TOOLS_DIR:-/root/sev-tools}
SRC=$TOP/src
BUILD=$TOP/build
PREFIX=$TOP/prefix
JOBS=${JOBS:-$(nproc)}

QEMU_VERSION=11.1.2
QEMU_SHA256=731b5681e4bb18be313231579b8efd0296c5b015fa36dc533874b639ba838016
QEMU_KEY_FPR=CEACC9E15534EBABB82D3FA03353C9CEF108B584   # Michael Roth (AMD), QEMU release signer

IGVM_TAG=igvm-v0.4.0
IGVM_SHA256=1ee7b61769254a59de1011ca3fe0b6a17cc8e8412a4f564d77c324a160b4eccf
CBINDGEN_VERSION=0.29.4

# Rust tools: name, GitHub repository, tag, SHA-256 of the tag's tarball.
RUST_TOOLS=(
    "snpguest virtee/snpguest v0.10.0 5f4452fc92c365d9e41098ec9e7ebeb46bdf0a7150dc81a6bb2e022ff295789e"
    "snphost virtee/snphost v0.7.0 9ddbb6f2a7a0aadc9bb951292ecfeda88c07b4d6339e1ff31b3ec609ef080159"
    "sevctl virtee/sevctl v0.6.2 e368417ed7170820951fe6d085745131fb009b3f5d326031998bc8796a19e7ae"
)
SEV_SNP_MEASURE_VERSION=0.0.13
# Toolchain for the Rust tools, installed with rustup into $SEV_TOOLS_DIR (the toolchain itself is
# checked by rustup against the channel manifest's hashes).
RUSTUP_VERSION=1.29.1
RUSTUP_INIT_SHA256=dda7234360b7f578ca8b0ddcb80145646fa61a67c1720a5abc7051b35c9fcb71
RUST_TOOLCHAIN=1.95.0
# Firmware: Ubuntu 26.04 (resolute-updates) edk2 packages; the SHA-256 values are those of the
# signed archive index (InRelease checked with gpgv against ubuntu-archive-keyring.gpg on
# 2026-10-16, dev docs: sev/FINDINGS.md).
OVMF_VERSION=2025.11-3ubuntu7.3
OVMF_DEBS=(
    "ovmf-amdsev f2a54db678d98ce1a5ac27155cb037cbf6e3efc918d11a35af1ca4606a290d29"
    "ovmf-generic d35554c74dccef60c12d63dda1120bf1ddc3a44054a1bf4b596f7dda7504c588"
)
GUEST_KERNEL=6.8.0-147-generic

export CARGO_HOME=$TOP/cargo
export PATH=$PREFIX/bin:$CARGO_HOME/bin:$PATH

log() { echo ">>> $*"; }

fetch() {   # fetch <url> <file> <sha256>
    local url=$1 file=$2 sha=$3
    mkdir -p "$SRC"
    if [ ! -f "$SRC/$file" ]; then
        curl -sSfL -o "$SRC/$file.part" "$url"
        mv "$SRC/$file.part" "$SRC/$file"
    fi
    local have
    have=$(sha256sum "$SRC/$file" | cut -d' ' -f1)
    if [ "$have" != "$sha" ]; then
        echo "error: $file: sha256 $have, expected $sha" >&2
        exit 1
    fi
}

dearmor() {   # ASCII-armored OpenPGP key -> binary keyring (gpgv reads binary keyrings; no gpg here)
    python3 -I -c '
import base64, sys
lines = open(sys.argv[1]).read().splitlines()
body = lines[lines.index("") + 1 if "" in lines else 1:]
body = [l for l in body if l and not l.startswith("-----") and not l.startswith("=")]
sys.stdout.buffer.write(base64.b64decode("".join(body)))
' "$1"
}

build_igvm() {
    fetch "https://github.com/microsoft/igvm/archive/refs/tags/$IGVM_TAG.tar.gz" "$IGVM_TAG.tar.gz" "$IGVM_SHA256"
    if ! command -v cbindgen >/dev/null; then
        log "cbindgen $CBINDGEN_VERSION (cargo install)"
        cargo install --locked --root "$PREFIX" --version "$CBINDGEN_VERSION" cbindgen
    fi
    rm -rf "$BUILD/igvm"
    mkdir -p "$BUILD/igvm"
    tar -xzf "$SRC/$IGVM_TAG.tar.gz" -C "$BUILD/igvm" --strip-components=1
    log "libigvm ($IGVM_TAG)"
    # igvm has no Cargo.lock in its tree; the dependency versions cargo resolves are recorded in
    # the build log. Only `build` (not `all`, which also runs its CUnit tests: no libcunit here).
    make -C "$BUILD/igvm" -f igvm_c/Makefile RELEASE=1 EXTRA_PARAMS=--release CFLAGS="-O2" build
    local ver
    ver=$(grep -m1 -oP '(?<=^version = ")[^"]+' "$BUILD/igvm/igvm/Cargo.toml")
    mkdir -p "$PREFIX/include/igvm" "$PREFIX/lib64/pkgconfig" "$PREFIX/bin"
    install -m 644 "$BUILD/igvm/target_c/release/libigvm.a" "$PREFIX/lib64/"
    install -m 644 "$BUILD/igvm/igvm_c/include/"* "$PREFIX/include/igvm/"
    install -m 755 "$BUILD/igvm/target_c/release/dump_igvm" "$PREFIX/bin/"
    sed -e "s|\${PREFIX}|$PREFIX|g" -e "s|\$PREFIX|$PREFIX|g" \
        -e "s|\${VERSION}|$ver|g" -e "s|\$VERSION|$ver|g" \
        "$BUILD/igvm/igvm_c/igvm.pc.in" > "$PREFIX/lib64/pkgconfig/igvm.pc"
    log "libigvm $ver installed in $PREFIX"
}

build_qemu() {
    local tarball=qemu-$QEMU_VERSION.tar.xz
    fetch "https://download.qemu.org/$tarball" "$tarball" "$QEMU_SHA256"
    [ -f "$SRC/$tarball.sig" ] || curl -sSfL -o "$SRC/$tarball.sig" "https://download.qemu.org/$tarball.sig"
    local keyring
    keyring=$(mktemp)
    dearmor "$HERE/qemu-release-key-$QEMU_KEY_FPR.asc" > "$keyring"
    local status
    status=$(gpgv --status-fd 1 --keyring "$keyring" "$SRC/$tarball.sig" "$SRC/$tarball" 2>/dev/null || true)
    rm -f "$keyring"
    if ! grep -q "^\[GNUPG:\] VALIDSIG $QEMU_KEY_FPR " <<<"$status"; then
        echo "error: $tarball: no valid signature by $QEMU_KEY_FPR" >&2
        exit 1
    fi
    log "$tarball: good signature by $QEMU_KEY_FPR, sha256 ok"
    [ -f "$PREFIX/lib64/pkgconfig/igvm.pc" ] || build_igvm
    rm -rf "$BUILD/qemu-$QEMU_VERSION"
    tar -xJf "$SRC/$tarball" -C "$BUILD"
    # local fixes, each explained in its header (plain SEV + IGVM launch measurement order)
    for p in "$HERE"/qemu-"$QEMU_VERSION"-*.patch; do
        [ -e "$p" ] || continue
        log "applying $(basename "$p")"
        patch -d "$BUILD/qemu-$QEMU_VERSION" -p1 < "$p"
    done
    mkdir -p "$BUILD/qemu-$QEMU_VERSION/build"
    cd "$BUILD/qemu-$QEMU_VERSION/build"
    log "QEMU $QEMU_VERSION (x86_64-softmmu, KVM, IGVM)"
    PKG_CONFIG_PATH=$PREFIX/lib64/pkgconfig ../configure --prefix="$PREFIX" \
        --target-list=x86_64-softmmu --enable-kvm --enable-igvm --enable-slirp \
        --disable-docs --disable-werror --disable-xen --disable-gtk --disable-sdl --disable-vnc \
        --disable-opengl --disable-virglrenderer
    make -j"$JOBS"
    make install
    "$PREFIX/bin/qemu-system-x86_64" --version | head -n 1
    "$PREFIX/bin/qemu-system-x86_64" -object help | grep -E 'sev|igvm'
}

rust_toolchain() {   # the image's cargo 1.85 is too old for snpguest 0.10 (rustc >= 1.86)
    if [ ! -x "$CARGO_HOME/bin/rustup" ]; then
        fetch "https://static.rust-lang.org/rustup/archive/$RUSTUP_VERSION/x86_64-unknown-linux-gnu/rustup-init" \
              "rustup-init-$RUSTUP_VERSION" "$RUSTUP_INIT_SHA256"
        chmod +x "$SRC/rustup-init-$RUSTUP_VERSION"
        "$SRC/rustup-init-$RUSTUP_VERSION" -y -q --no-modify-path --profile minimal \
            --default-toolchain "$RUST_TOOLCHAIN"
    fi
    "$CARGO_HOME/bin/rustup" toolchain install --profile minimal "$RUST_TOOLCHAIN"
}

build_tools() {
    local entry name repo tag sha
    export RUSTUP_HOME=$TOP/rustup RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN
    rust_toolchain
    cargo --version
    for entry in "${RUST_TOOLS[@]}"; do
        read -r name repo tag sha <<<"$entry"
        fetch "https://github.com/$repo/archive/refs/tags/$tag.tar.gz" "$name-$tag.tar.gz" "$sha"
        rm -rf "$BUILD/$name"
        mkdir -p "$BUILD/$name"
        tar -xzf "$SRC/$name-$tag.tar.gz" -C "$BUILD/$name" --strip-components=1
        log "$name $tag (cargo install --locked)"
        cargo install --locked --root "$PREFIX" --path "$BUILD/$name"
    done
    log "sev-snp-measure $SEV_SNP_MEASURE_VERSION (venv)"
    python3 -m venv "$PREFIX/venv-sev-snp-measure"
    "$PREFIX/venv-sev-snp-measure/bin/pip" install -q "sev-snp-measure==$SEV_SNP_MEASURE_VERSION"
    ln -sf ../venv-sev-snp-measure/bin/sev-snp-measure "$PREFIX/bin/sev-snp-measure"
    for t in snpguest snphost sevctl sev-snp-measure; do
        printf '%-16s ' "$t"; "$PREFIX/bin/$t" --version 2>&1 | head -n 1
    done
}

get_firmware() {
    local entry name sha out=$TOP/firmware/edk2-${OVMF_VERSION%%-*}
    for entry in "${OVMF_DEBS[@]}"; do
        read -r name sha <<<"$entry"
        fetch "http://archive.ubuntu.com/ubuntu/pool/main/e/edk2/${name}_${OVMF_VERSION}_all.deb" \
              "${name}_${OVMF_VERSION}_all.deb" "$sha"
        rm -rf "$BUILD/$name"
        dpkg-deb -x "$SRC/${name}_${OVMF_VERSION}_all.deb" "$BUILD/$name"
    done
    mkdir -p "$out"
    install -m 644 "$BUILD"/ovmf-*/usr/share/ovmf/OVMF.amdsev.fd "$BUILD"/ovmf-*/usr/share/ovmf/OVMF.fd "$out/"
    sha256sum "$out"/*.fd
}

get_guest() {   # the packages come from the container's apt sources (Ubuntu 24.04, signed index)
    local g=$TOP/guest
    mkdir -p "$g/debs"
    (cd "$g/debs" && apt-get update -qq && apt-get download -qq "linux-image-$GUEST_KERNEL" \
        "linux-modules-extra-$GUEST_KERNEL" busybox-static)
    rm -rf "$g/root"
    for d in "$g"/debs/linux-image-"$GUEST_KERNEL"_*.deb "$g"/debs/linux-modules-extra-"$GUEST_KERNEL"_*.deb \
             "$g"/debs/busybox-static_*.deb; do
        dpkg-deb -x "$d" "$g/root"
    done
    install -m 644 "$g/root/boot/vmlinuz-$GUEST_KERNEL" "$g/vmlinuz"
    # sev-guest (SNP guest requests, configfs-tsm reports) is a module; busybox insmod cannot read
    # .ko.zst and the image has no zstd, so decompress with the zstandard module in a venv.
    [ -x "$TOP/venv-zstd/bin/python" ] || { python3 -m venv "$TOP/venv-zstd"; "$TOP/venv-zstd/bin/pip" install -q zstandard; }
    local m=$g/root/lib/modules/$GUEST_KERNEL/kernel/drivers/virt/coco
    "$TOP/venv-zstd/bin/python" -I -c '
import sys, zstandard
for src, dst in zip(sys.argv[1::2], sys.argv[2::2]):
    open(dst, "wb").write(zstandard.ZstdDecompressor().stream_reader(open(src, "rb")).read())
' "$m/tsm.ko.zst" "$g/1-tsm.ko" "$m/sev-guest/sev-guest.ko.zst" "$g/2-sev-guest.ko"
    "$HERE/make-smoke-initrd.py" "$g/root/usr/bin/busybox" "$g/smoke-initrd.cpio" \
        "lib/modules/1-tsm.ko=$g/1-tsm.ko" "lib/modules/2-sev-guest.ko=$g/2-sev-guest.ko"
}

mkdir -p "$SRC" "$BUILD" "$PREFIX"
case ${1:-all} in
    igvm)     build_igvm ;;
    qemu)     build_qemu ;;
    tools)    build_tools ;;
    firmware) get_firmware ;;
    guest)    get_guest ;;
    all)      build_igvm; build_qemu; build_tools; get_firmware; get_guest ;;
    *) echo "usage: $0 [igvm|qemu|tools|firmware|guest|all]" >&2; exit 2 ;;
esac
