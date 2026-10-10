#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# install.sh — build and install Gramine, set up the environment for using it, and optionally run
# the test battery. Works from any directory.
#
# Usage:
#   scripts/install.sh [options] [suite...]
#
# Options:
#   --prefix DIR           install prefix (default: $HOME/.local/gramine; no root needed)
#   --build-dir DIR        meson build directory, relative to the repository root
#                          (default: build-$(uname -m))
#   --deps                 install the build dependencies first (scripts/dependencies.sh
#                          --build-only; done automatically if build tools are missing)
#   --add-to-profile       also source <prefix>/env.sh from ~/.bashrc, so new shells have Gramine
#                          on PATH (idempotent)
#   --sgx | --vm | --tdx   enable extra PAL backends (off by default; "direct" is always built)
#   --graphics enabled|disabled
#                          compile the Tirith graphics host-redirection paths into the LibOS
#                          (default: enabled)
#   -j N                   ninja parallelism (default: nproc)
#   --run-tests [suite...] run scripts/test.sh afterwards, building the test manifests first
#                          (naming suites implies this)
#   -h, --help
#
# Environment: after installing, <prefix>/env.sh is written. A script cannot change the environment
# of the shell that started it, so use the install in your current shell with
#     . <prefix>/env.sh
# (or pass --add-to-profile to get it in every new shell). scripts/test.sh and the other scripts
# in this directory find the install by themselves (GRAMINE_PREFIX, default ~/.local/gramine).
#
# --vm builds the vm-PAL unikernel ("the correct vm version for this machine"): the default build
# dir becomes build-vm-<arch> so it never reconfigures a direct-PAL build dir, and on aarch64 the
# option set is fixed to the README-vm.md recipe (-Dvm=enabled, direct/sgx/tdx/tests all disabled --
# the aarch64 vm kernel replaces the direct PAL entirely, so a direct-PAL test battery does not
# apply). On x86_64 your --sgx/--tdx choices are kept.
#
# Defaults match this repo's tested configuration:
#   -Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dtdx=disabled -Dgraphics=enabled
#   -Dtests=enabled -Dlibc=glibc -Dbuildtype=release
#
# Re-running is cheap and safe: meson reuses a configured build dir with matching options and
# ninja rebuilds only what changed.
#
# Notes:
#  * Dependencies come from scripts/dependencies.sh (apt, sudo, may prompt for a password).
#  * vm/tdx are Tirith forks of the PAL that need extra system dependencies (and SGX/TDX hardware);
#    they stay disabled for the direct-PAL battery.
#  * --vm on aarch64 is self-contained: no SGX/TDX packages are needed, and the build runs wherever
#    meson/gcc exist; only *running* the resulting kernel needs QEMU with KVM.

set -euo pipefail

script_dir=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$script_dir")
cd "$REPO"

PREFIX="$HOME/.local/gramine"
ARCH=$(uname -m)
BUILDDIR="build-$ARCH"
BUILDDIR_EXPLICIT=0
DO_DEPS=0
ADD_TO_PROFILE=0
RUN_TESTS=0
JOBS="$(nproc)"
SGX=disabled VM=disabled TDX=disabled
GRAPHICS=enabled   # Tirith graphics paths in the LibOS (see meson_options.txt)
TEST_SUITES=()

usage() { sed -n '/^# install.sh/,/^# Notes:/p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)        usage; exit 0 ;;
        --prefix)         PREFIX=$2; shift 2 ;;
        --build-dir)      BUILDDIR=$2; BUILDDIR_EXPLICIT=1; shift 2 ;;
        --deps)           DO_DEPS=1; shift ;;
        --add-to-profile) ADD_TO_PROFILE=1; shift ;;
        --run-tests)      RUN_TESTS=1; shift ;;
        --sgx)            SGX=enabled; shift ;;
        --vm)             VM=enabled; shift ;;
        --tdx)            TDX=enabled; shift ;;
        --graphics)       GRAPHICS=$2; shift 2 ;;
        -j*)              JOBS=${1#-j}; [ -n "$JOBS" ] || { JOBS=$2; shift; }; shift ;;
        pal|libos|helloworld|multithread|busybox|redis|memcached)
                          RUN_TESTS=1; TEST_SUITES+=("$1"); shift ;;
        *)                echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
done

PREFIX=$(realpath -m "$PREFIX")

# --vm: build the vm-PAL unikernel ("the correct vm version" for this machine).
DIRECT=enabled
TESTS=enabled
if [ "$VM" = enabled ]; then
    # Never share (and thus never reconfigure) a direct-PAL build dir.
    if [ "$BUILDDIR_EXPLICIT" = 0 ]; then
        BUILDDIR="build-vm-$ARCH"
    fi
    if [ "$ARCH" = aarch64 ]; then
        # aarch64 recipe (README-vm.md): the vm kernel *is* the PAL here; the direct PAL and
        # SGX/TDX hosts do not exist in this build, and the PAL regression battery is built
        # against the direct PAL.
        if [ "$SGX" = enabled ] || [ "$TDX" = enabled ]; then
            echo "note: --sgx/--tdx have no aarch64 vm host; forcing them off"
        fi
        SGX=disabled TDX=disabled
        DIRECT=disabled
        TESTS=disabled
    fi
fi

missing_tools() {
    for tool in meson ninja python3 cc make pkg-config bison; do
        command -v "$tool" >/dev/null || return 0
    done
    return 1
}

# Install build dependencies when asked (--deps) or when build tools are missing.
if [ "$DO_DEPS" = 1 ] || missing_tools; then
    echo ">>> installing build dependencies (sudo may ask for your password)"
    "$script_dir/dependencies.sh" --build-only
fi

for tool in meson ninja python3 cc make bison; do
    command -v "$tool" >/dev/null || { echo "ERROR: '$tool' not found after dependency install"; exit 1; }
done

echo ">>> configuring ($BUILDDIR -> $PREFIX, direct=$DIRECT sgx=$SGX vm=$VM tdx=$TDX graphics=$GRAPHICS tests=$TESTS)"
# On an already-configured build dir this reconfigures in place (cheap);
# --reconfigure covers the case where meson insists on regeneration.
if ! meson setup "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect="$DIRECT" \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" -Dgraphics="$GRAPHICS" \
        -Dtests="$TESTS" \
        -Dlibc=glibc 2>"$BUILDDIR-setup.err"; then
    echo "    plain 'meson setup' failed; retrying with --reconfigure"
    meson setup --reconfigure "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect="$DIRECT" \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" -Dgraphics="$GRAPHICS" \
        -Dtests="$TESTS" \
        -Dlibc=glibc \
    || { echo "ERROR: meson setup failed:"; tail -5 "$BUILDDIR-setup.err"; exit 1; }
fi
rm -f "$BUILDDIR-setup.err"

echo ">>> building (-j$JOBS)"
ninja -C "$BUILDDIR" -j "$JOBS"

echo ">>> installing to $PREFIX"
ninja -C "$BUILDDIR" -j "$JOBS" install

# --- environment setup -------------------------------------------------------------------------
# <prefix>/env.sh is self-contained (it does not depend on this repository's location).
SITE_PACKAGES=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -n 1 || true)
{
    echo "# Generated by scripts/install.sh: environment for the Gramine install in this directory."
    echo "# Use it with:  . \"$PREFIX/env.sh\""
    echo "export GRAMINE_PREFIX=\"$PREFIX\""
    echo "export PATH=\"\$GRAMINE_PREFIX/bin:\$PATH\""
    if [ -n "$SITE_PACKAGES" ]; then
        echo "export PYTHONPATH=\"$SITE_PACKAGES\${PYTHONPATH:+:\$PYTHONPATH}\""
    fi
} > "$PREFIX/env.sh"
echo ">>> wrote $PREFIX/env.sh"

if [ "$ADD_TO_PROFILE" = 1 ]; then
    PROFILE="$HOME/.bashrc"
    touch "$PROFILE"
    sed -i '/# >>> gramine-multiarch >>>/,/# <<< gramine-multiarch <<</d' "$PROFILE"
    {
        echo "# >>> gramine-multiarch >>>"
        echo ". \"$PREFIX/env.sh\""
        echo "# <<< gramine-multiarch <<<"
    } >> "$PROFILE"
    echo ">>> added $PREFIX/env.sh to $PROFILE (open a new shell, or run:  . \"$PREFIX/env.sh\")"
fi

if [ "$RUN_TESTS" = 1 ]; then
    if [ "$VM" = enabled ] && [ "$ARCH" = aarch64 ]; then
        echo "note: --run-tests skipped -- the regression battery needs the direct PAL,"
        echo "      which a --vm aarch64 build does not build."
    else
        echo ">>> running the test battery"
        export GRAMINE_PREFIX="$PREFIX"
        exec "$script_dir/test.sh" ${TEST_SUITES[@]+"${TEST_SUITES[@]}"}
    fi
fi

echo
if [ "$VM" = enabled ] && [ "$ARCH" = aarch64 ]; then
    echo "VM artifacts:"
    echo "  kernel: $REPO/$BUILDDIR/pal/src/host/vm/pal"
    echo "  LibOS:  $REPO/$BUILDDIR/libos/src/libsysdb.so"
fi
echo "Done. To use this install in the current shell:   . \"$PREFIX/env.sh\""
[ "$ADD_TO_PROFILE" = 1 ] || echo "(or rerun with --add-to-profile to get it in every new shell)"
echo "Test with:  GRAMINE_PREFIX=\"$PREFIX\" scripts/test.sh"
