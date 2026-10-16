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
#   --target ARCH          cross-build for ARCH instead of the build machine's own
#                          architecture (riscv64; see Documentation/devel/riscv/PLAN.md).
#                          Requires the matching cross toolchain to be installed -- for
#                          riscv64 that is the `container/run.sh --target riscv64` container
#                          (or the same packages on a native-capable distro). The default
#                          build dir and prefix gain a -riscv64 suffix so a cross build can
#                          never clobber the native one.
#   --build-dir DIR        meson build directory, relative to the repository root
#                          (default: build-$(uname -m), or build-riscv64 with --target)
#   --deps                 install the build dependencies first (scripts/dependencies.sh
#                          --build-only; done automatically if build tools are missing)
#   --add-to-profile       also source <prefix>/env.sh from ~/.bashrc, so new shells have Gramine
#                          on PATH (idempotent)
#   --sgx | --vm | --tdx   enable extra PAL backends (off by default; "direct" is always built)
#   --sev                  --vm plus the AMD SEV build of the vm PAL and gramine-sev (x86_64;
#                          Documentation/devel/sev/)
#   --graphics enabled|disabled
#                          compile the Tirith graphics host-redirection paths into the LibOS
#                          (default: enabled)
#   --libc glibc|none|musl which libc is built into the runtime directory that manifests mount
#                          at /lib. `none` builds no libc (much shorter build): applications run
#                          on the host's glibc. Default: none for a --vm build and for direct
#                          aarch64/riscv64 builds, without --sgx or --tdx; glibc otherwise
#                          (x86_64 direct keeps the patched glibc). SGX needs the bundled glibc.
#   -j N                   ninja parallelism (default: nproc)
#   --run-tests [suite...] run test/test-direct.sh afterwards, building the test manifests first
#                          (with --vm on aarch64: test/test-vm.sh, suites libos|examples|hello)
#                          (naming suites implies this)
#   -h, --help
#
# Environment: after installing, <prefix>/env.sh is written. A script cannot change the environment
# of the shell that started it, so use the install in your current shell with
#     . <prefix>/env.sh
# (or pass --add-to-profile to get it in every new shell). test/test-direct.sh and the other scripts
# in this directory find the install by themselves (GRAMINE_PREFIX, default ~/.local/gramine).
#
# --vm builds the vm-PAL unikernel ("the correct vm version for this machine"): the default build
# dir becomes build-vm-<arch> so it never reconfigures a direct-PAL build dir, and on aarch64 the
# option set is fixed to the Documentation/devel/vm.md recipe (-Dvm=enabled, direct/sgx/tdx/tests all disabled --
# the aarch64 vm kernel replaces the direct PAL entirely, so a direct-PAL test battery does not
# apply). On x86_64 your --sgx/--tdx choices are kept.
#
# Defaults match this repo's tested configuration:
#   -Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dtdx=disabled -Dgraphics=enabled
#   -Dtests=enabled -Dlibc=glibc -Dbuildtype=release   (libc=none for --vm, aarch64 and riscv64, see --libc)
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
#  * --target riscv64 only *builds* (and installs); riscv64 binaries cannot run on this build
#    machine's kernel. Run the test battery inside the RISC-V guest instead
#    (scripts/riscv64/guest.sh + test/test-direct.sh there, see the plan's Phase 0 notes).
#    --deps is skipped for cross builds: the cross toolchain comes from the container image,
#    not from scripts/dependencies.sh.

set -euo pipefail

script_dir=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$script_dir")
cd "$REPO"

PREFIX="$HOME/.local/gramine"
PREFIX_EXPLICIT=0
TESTS_PIE=false
TARGET_ARCH=""
ARCH=$(uname -m)
BUILDDIR="build-$ARCH"
BUILDDIR_EXPLICIT=0
DO_DEPS=0
ADD_TO_PROFILE=0
RUN_TESTS=0
JOBS="$(nproc)"
SGX=disabled VM=disabled TDX=disabled SEV=disabled
GRAPHICS=enabled   # Tirith graphics paths in the LibOS (see meson_options.txt)
LIBC=              # empty: chosen after the options are parsed (see --libc)
TEST_SUITES=()

usage() { sed -n '/^# install.sh/,/^# Notes:/p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)        usage; exit 0 ;;
        --prefix)         PREFIX=$2; PREFIX_EXPLICIT=1; shift 2 ;;
        --target)         TARGET_ARCH=$2; shift 2 ;;
        --build-dir)      BUILDDIR=$2; BUILDDIR_EXPLICIT=1; shift 2 ;;
        --deps)           DO_DEPS=1; shift ;;
        --add-to-profile) ADD_TO_PROFILE=1; shift ;;
        --run-tests)      RUN_TESTS=1; shift ;;
        --sgx)            SGX=enabled; shift ;;
        --vm)             VM=enabled; shift ;;
        --tdx)            TDX=enabled; shift ;;
        --sev)            SEV=enabled; VM=enabled; shift ;;
        --graphics)       GRAPHICS=$2; shift 2 ;;
        --libc)           LIBC=$2; shift 2 ;;
        -j*)              JOBS=${1#-j}; [ -n "$JOBS" ] || { JOBS=$2; shift; }; shift ;;
        pal|libos|helloworld|multithread|busybox|redis|memcached|examples|hello)
                          RUN_TESTS=1; TEST_SUITES+=("$1"); shift ;;
        *)                echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
done

# --target: cross-build. Only riscv64 (the direct PAL port,
# Documentation/devel/riscv/PLAN.md) is supported here.
CROSS_ARGS=()
if [ -n "$TARGET_ARCH" ]; then
    if [ "$TARGET_ARCH" != riscv64 ]; then
        echo "ERROR: --target only supports 'riscv64' (got '$TARGET_ARCH')"
        exit 2
    fi
    ARCH=$TARGET_ARCH
    if [ "$BUILDDIR_EXPLICIT" = 0 ]; then
        # never share (and thus never reconfigure) the native build dir
        BUILDDIR="build-riscv64"
    fi
    if [ "$PREFIX_EXPLICIT" = 0 ]; then
        # ...and never overwrite a native install either (same file names)
        PREFIX="$HOME/.local/gramine-riscv64"
    fi
    CROSS_ARGS=(--cross-file "$script_dir/riscv64/meson-cross.ini")
fi

# --vm: build the vm-PAL unikernel ("the correct vm version" for this machine).
# (PREFIX is normalized below, after the vm default is applied.)

# --vm: build the vm-PAL unikernel ("the correct vm version" for this machine).
DIRECT=enabled
TESTS=enabled
if [ "$VM" = enabled ]; then
    # Never share (and thus never reconfigure) a direct-PAL build dir.
    if [ "$BUILDDIR_EXPLICIT" = 0 ]; then
        BUILDDIR="build-vm-$ARCH"
    fi
    # ...and never overwrite a direct-PAL install either (same file names, different PAL)
    if [ "$PREFIX_EXPLICIT" = 0 ]; then
        PREFIX="$HOME/.local/gramine-vm"
    fi
    if [ "$ARCH" = aarch64 ]; then
        # aarch64 recipe (Documentation/devel/vm.md): the vm kernel *is* the PAL here; the direct PAL and
        # SGX/TDX hosts do not exist in this build. The PAL regression battery is built against
        # the direct PAL and is not built; the LibOS regression suite runs under gramine-vm
        # (test/test-vm.sh), which needs PIE test binaries (the guest maps DRAM at 0x40000000).
        if [ "$SGX" = enabled ] || [ "$TDX" = enabled ]; then
            echo "note: --sgx/--tdx have no aarch64 vm host; forcing them off"
        fi
        SGX=disabled TDX=disabled
        DIRECT=disabled
        TESTS=enabled
        TESTS_PIE=true
        # graphics redirection needs the QEMU fork's host agent (sg-listener), which does not exist
        # for aarch64 (stock QEMU is the requirement): without an agent the redirected fd paths
        # misbehave (e.g. close() of an invalid fd returns 0 instead of EBADF).
        GRAPHICS=disabled
    fi
fi

# --libc: a --vm build and direct aarch64/riscv64 builds default to no bundled libc (applications
# run on the host's glibc; these direct PALs do not need a patched glibc).
# x86_64 direct builds keep the bundled, patched glibc (it avoids the seccomp trap per syscall), and
# SGX needs it.
if [ -z "$LIBC" ]; then
    if { [ "$VM" = enabled ] || [ "$ARCH" = aarch64 ] || [ "$ARCH" = riscv64 ]; } \
        && [ "$SGX" != enabled ] && [ "$TDX" != enabled ]; then
        LIBC=none
    else
        LIBC=glibc
    fi
fi
case "$LIBC" in
    glibc|none|musl) ;;
    *) echo "ERROR: --libc must be glibc, none or musl (got '$LIBC')"; exit 2 ;;
esac
if [ "$LIBC" = none ] && [ "$SGX" = enabled ]; then
    echo "ERROR: --sgx needs the bundled glibc; use --libc glibc"
    exit 2
fi

PREFIX=$(realpath -m "$PREFIX")

missing_tools() {
    for tool in meson ninja python3 cc make pkg-config bison; do
        command -v "$tool" >/dev/null || return 0
    done
    return 1
}

# Install build dependencies when asked (--deps) or when build tools are missing.
# Cross builds get their toolchain from the cross container image (or an equivalently
# provisioned machine); scripts/dependencies.sh only knows the build machine's distro
# packages, so it is skipped.
if [ -n "$TARGET_ARCH" ] && [ "$DO_DEPS" = 1 ]; then
    echo "note: --deps is a no-op for --target $TARGET_ARCH (toolchain comes from the container)"
    DO_DEPS=0
fi
if [ "$DO_DEPS" = 1 ] || missing_tools; then
    echo ">>> installing build dependencies (sudo may ask for your password)"
    "$script_dir/dependencies.sh" --build-only
fi

for tool in meson ninja python3 cc make bison; do
    command -v "$tool" >/dev/null || { echo "ERROR: '$tool' not found after dependency install"; exit 1; }
done

echo ">>> configuring ($BUILDDIR -> $PREFIX, direct=$DIRECT sgx=$SGX vm=$VM tdx=$TDX sev=$SEV graphics=$GRAPHICS libc=$LIBC tests=$TESTS)"
# On an already-configured build dir this reconfigures in place (cheap);
# --reconfigure covers the case where meson insists on regeneration.
if ! meson setup "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect="$DIRECT" \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" -Dsev="$SEV" -Dgraphics="$GRAPHICS" \
        -Dtests="$TESTS" -Dtests_pie="$TESTS_PIE" \
        -Dlibc="$LIBC" ${CROSS_ARGS[@]+"${CROSS_ARGS[@]}"} \
        2>"$BUILDDIR-setup.err"; then
    echo "    plain 'meson setup' failed; retrying with --reconfigure"
    meson setup --reconfigure "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect="$DIRECT" \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" -Dsev="$SEV" -Dgraphics="$GRAPHICS" \
        -Dtests="$TESTS" -Dtests_pie="$TESTS_PIE" \
        -Dlibc="$LIBC" ${CROSS_ARGS[@]+"${CROSS_ARGS[@]}"} \
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

if [ "$RUN_TESTS" = 1 ] && [ -n "$TARGET_ARCH" ]; then
    echo "note: skipping --run-tests: $TARGET_ARCH binaries cannot run on this build machine;"
    echo "      run the battery inside the RISC-V guest instead (scripts/riscv64/guest.sh,"
    echo "      then test/test-direct.sh there -- see Documentation/devel/riscv/PLAN.md Phase 0)"
    RUN_TESTS=0
fi

if [ "$RUN_TESTS" = 1 ]; then
    export GRAMINE_PREFIX="$PREFIX"
    if [ "$VM" = enabled ] && [ "$ARCH" = aarch64 ]; then
        echo ">>> running the vm test battery"
        exec "$REPO/test/test-vm.sh" --build-dir "$BUILDDIR" ${TEST_SUITES[@]+"${TEST_SUITES[@]}"}
    else
        echo ">>> running the test battery"
        exec "$REPO/test/test-direct.sh" ${TEST_SUITES[@]+"${TEST_SUITES[@]}"}
    fi
fi

echo
if [ "$VM" = enabled ] && [ "$ARCH" = aarch64 ]; then
    echo "VM artifacts:"
    echo "  kernel: $REPO/$BUILDDIR/pal/src/host/vm/pal"
    echo "  LibOS:  $REPO/$BUILDDIR/libos/src/libsysdb.so"
fi
if [ -n "$TARGET_ARCH" ]; then
    echo "Done (cross $TARGET_ARCH build). Artifacts live in $REPO/$BUILDDIR; use them in the"
    echo "RISC-V guest (install there or export $BUILDDIR over 9p), not on this machine."
fi
echo "Done. To use this install in the current shell:   . \"$PREFIX/env.sh\""
[ "$ADD_TO_PROFILE" = 1 ] || echo "(or rerun with --add-to-profile to get it in every new shell)"
if [ "$VM" = enabled ] && [ "$ARCH" = aarch64 ]; then
    echo "Test with:  test/test-vm.sh --prefix \"$PREFIX\" --build-dir $BUILDDIR"
else
    echo "Test with:  GRAMINE_PREFIX=\"$PREFIX\" test/test-direct.sh"
fi
