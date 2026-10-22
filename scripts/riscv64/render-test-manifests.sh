#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Render direct-PAL test manifests for the riscv64 cross build.
#
# A native build runs `gramine-test build` inside the suite directory: it generates a
# build.ninja that compiles the test binaries *and* renders each test's manifest from its
# template (falling back to the suite's shared `manifest.template`) next to the binary, and
# pytest then runs with that directory as CWD (test binaries are invoked by bare name, so
# `<name>.manifest` is looked up in CWD).
#
# On the cross build there is no native compiler in the guest, but the binaries already
# exist in the meson build tree. So this script -- run inside the build container --
# creates a per-suite *run directory* under the build tree, symlinks the cross-built
# binaries (and any committed data files) into it, and renders the manifests there with
# the same gramine-manifest invocation and the same variable set that
# `graminelibos.util_tests._gen_rules` uses. pytest is then run in the guest with CWD =
# run directory and the test file given by path.
#
# Usage (in the container): render-test-manifests.sh <pal|libos>
set -euo pipefail

SUITE="${1:?usage: render-test-manifests.sh <pal|libos>}"
REPO="${REPO:-/root/gramine-multiarch}"
BUILD="${BUILD:-$REPO/build-riscv64}"
PREFIX="${PREFIX:-$BUILD/prefix}"

# same value the fork's ninja rule passes; the guest's own multiarch libdir, which the
# default template mounts (and lists libgcc_s/libstdc++ from in trusted_files).
ARCH_LIBDIR=/lib/riscv64-linux-gnu

# With libc=none, gramine.runtimedir() uses SAGUARO_HOST_LIBC_DIR. This renderer runs in the
# x86_64 cross container, but the manifests are executed in the RISC-V guest; do not let the
# renderer's MULTIARCH (or any cross sysroot) select the runtime directory. An alternate guest
# sysroot can override this value explicitly.
SAGUARO_HOST_LIBC_DIR=${SAGUARO_HOST_LIBC_DIR:-/lib/riscv64-linux-gnu}
export SAGUARO_HOST_LIBC_DIR

case "$SUITE" in
    pal)
        SRC="$REPO/pal/regression"
        BINS="$BUILD/pal/regression"
        NO_CHECK="--no-check"   # PAL-level manifests omit [libos] (see tests.toml)
        ;;
    libos)
        SRC="$REPO/libos/test/regression"
        BINS="$BUILD/libos/test/regression"
        NO_CHECK=
        ;;
    *) echo "unknown suite: $SUITE" >&2; exit 2 ;;
esac

# Nest the run dir as run/<suite>/regression: several tests open `file:../regression/<name>.manifest`
# (File.c Open Test 3, the Directory test), which upstream resolves to the run dir itself when
# running in-tree at <build>/pal/regression -- `run/<suite>/regression/../regression` reproduces
# that self-reference exactly. (basename "$SRC" is "regression" for both suites, hence the <suite>
# level anyway.)
RUN="$BUILD/run/$SUITE/regression"
rm -rf "$RUN"
mkdir -p "$RUN"

export PYTHONPATH="$PREFIX/lib/python3.12/site-packages"
MANIFEST="$PREFIX/bin/gramine-manifest"

rendered=0
for bin in "$BINS"/* "$BINS"/.[!.]* "$BINS"/..?*; do
    [ -f "$bin" ] || continue
    name=$(basename "$bin")
    # skip build cruft: object dirs (*.p), sources, configs, anything with an extension
    # that is not a rendered ELF executable. Executables in the meson test dirs are the
    # bare, extension-less names the pytest driver invokes -- except the deliberate
    # `..Bootstrap` test binary (test_103_dotdot), which starts with dots and must NOT be
    # treated as extension-suffixed.
    case "$name" in
        .*) ;;
        *.*) continue ;;
    esac
    [ -x "$bin" ] || continue
    head -c4 "$bin" | grep -q $'\x7fELF' || continue

    tpl="$SRC/$name.manifest.template"
    [ -f "$tpl" ] || tpl="$SRC/manifest.template"

    ln -sf "$bin" "$RUN/$name"
    "$MANIFEST" $NO_CHECK \
        -Dbinary_dir="$RUN" \
        -Dentrypoint="$name" \
        -Darch_libdir="$ARCH_LIBDIR" \
        -Dlibc=glibc \
        -Dcoreutils_libdir=lib \
        "$tpl" "$RUN/$name.manifest"
    rendered=$((rendered + 1))
done

# committed (non-compiled) data files the tests read from CWD (e.g. scripts/*.sh for the
# exec_script test).
# Manifests that have no test binary of the same name (they name their entrypoint themselves,
# e.g. toml_parsing, env_from_host, debug_log_inline).
for tpl in "$SRC"/*.manifest.template; do
    [ -f "$tpl" ] || continue
    name=$(basename "$tpl" .manifest.template)
    [ -e "$RUN/$name.manifest" ] && continue
    "$MANIFEST" $NO_CHECK \
        -Dbinary_dir="$RUN" \
        -Dentrypoint="$name" \
        -Darch_libdir="$ARCH_LIBDIR" \
        -Dlibc=glibc \
        -Dcoreutils_libdir=lib \
        "$tpl" "$RUN/$name.manifest"
    rendered=$((rendered + 1))
done

(cd "$SRC" && git ls-files 2>/dev/null || true) | while read -r f; do
    case "$f" in
        *.c|*.h|*.cpp|*.py|*.toml|*.template|*.S|*.nasm|Makefile|meson.build|*.gdb|*.md) ;;
        *) if [ -e "$SRC/$f" ]; then
               mkdir -p "$(dirname "$RUN/$f")"
               ln -sfn "$SRC/$f" "$RUN/$f"
           fi ;;
    esac
done

echo "$SUITE: rendered $rendered manifests into $RUN"
