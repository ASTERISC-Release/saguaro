#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# The Linux side of the Windows-only VBS build (Documentation/devel/vbs-enclave/PLAN-windows-build.md)
# runs in a WSL2 Ubuntu distribution of the machine's architecture (aarch64 on ARM64 Windows, x86_64
# on x64 Windows; tested: Ubuntu 24.04 aarch64, Ubuntu 26.04 x86_64). This script checks it for the
# tools the build needs and installs what is missing, only when asked:
#
#   wsl-setup.sh check          list what is missing (exit 1 if anything is)
#   wsl-setup.sh apt            print the apt-get command that installs it system-wide (run it
#                               yourself with sudo; nothing is installed by this script)
#   wsl-setup.sh user           no sudo: download the same Ubuntu packages with `apt-get download`
#                               and unpack them into a user prefix ($GRAMINE_VBS_TOOLS, default
#                               ~/.local/share/gramine-vbs/tools); env.sh there sets PATH etc.
#   wsl-setup.sh env            print the path of env.sh (source it before building)
#
# With a second argument `clang`, clang is included (the Windows objects compiled in WSL).
#
# Packages: the build part of container/Containerfile.vm-aarch64 that the vbs build uses.
set -e

TOOLS=${GRAMINE_VBS_TOOLS:-$HOME/.local/share/gramine-vbs/tools}
# package -> a command or Python module that shows it is usable
PKGS="gcc:gcc make:make patch:patch git:git gawk:gawk bison:bison m4:m4 pkgconf:pkg-config
      meson:meson ninja-build:ninja python3:python3 python3-click:py.click
      python3-jinja2:py.jinja2 python3-tomli:py.tomli python3-tomli-w:py.tomli_w
      python3-voluptuous:py.voluptuous python3-pyelftools:py.elftools"
# x86_64: the LibOS and common sources have nasm files
[ "$(uname -m)" = x86_64 ] && PKGS="$PKGS nasm:nasm"
# libraries the packages above need that a minimal Ubuntu may lack (only for the user prefix)
DEPS="pkgconf-bin libpkgconf3"
if [ "$2" = clang ]; then
    PKGS="$PKGS clang:clang"
    # the versioned packages behind `clang` (18 on Ubuntu 24.04, 21 on 26.04)
    CV=$(apt-cache depends clang 2>/dev/null | sed -n 's/.*Depends: clang-\([0-9][0-9]*\)$/\1/p' | head -1)
    CV=${CV:-18}
    DEPS="$DEPS clang-$CV libclang-common-$CV-dev libclang-cpp$CV libclang1-$CV libllvm$CV llvm-$CV-linker-tools"
fi
ARCH=$(uname -m)
MULTIARCH=$(gcc -dumpmachine 2>/dev/null || echo "$ARCH-linux-gnu")

[ -f "$TOOLS/env.sh" ] && . "$TOOLS/env.sh"

usable() {
    case "$1" in
        py.*) python3 -c "import ${1#py.}" 2>/dev/null ;;
        *) command -v "$1" >/dev/null 2>&1 ;;
    esac
}

missing() {
    for e in $PKGS; do
        usable "${e#*:}" || printf '%s ' "${e%%:*}"
    done
}

case "${1:-check}" in
check)
    case "$ARCH" in aarch64|x86_64) ;; *) echo "not an aarch64 or x86_64 distribution ($ARCH)"; exit 1 ;; esac
    m=$(missing)
    if [ -n "$m" ]; then
        echo "missing: $m"
        echo "install: sudo apt-get install -y --no-install-recommends $m"
        echo "     or: $0 user${2:+ $2}   (no sudo; unpacks the packages into $TOOLS)"
        exit 1
    fi
    echo "ok: $(gcc -dumpfullversion) gcc, meson $(meson --version), ninja $(ninja --version)," \
         "$(python3 -V)"
    ;;
apt)
    m=$(missing)
    [ -z "$m" ] && { echo "nothing is missing"; exit 0; }
    echo "sudo apt-get update && sudo apt-get install -y --no-install-recommends $m"
    ;;
user)
    m=$(missing)
    [ -z "$m" ] && { echo "nothing is missing"; exit 0; }
    get=""
    for p in $m $DEPS; do
        dpkg -s "$p" >/dev/null 2>&1 && continue
        # a library this release does not have (or names differently) is not needed from here
        apt-cache show "$p" >/dev/null 2>&1 || { echo "skipping $p (not in this release)"; continue; }
        get="$get $p"
    done
    mkdir -p "$TOOLS/debs"
    echo "downloading:$get"
    (cd "$TOOLS/debs" && apt-get download $get)
    for d in "$TOOLS"/debs/*.deb; do dpkg -x "$d" "$TOOLS/root"; done
    cat > "$TOOLS/env.sh" <<ENV
# user-level tools of the vbs build (scripts/vbs/wsl-setup.sh user)
export PATH="$TOOLS/root/usr/bin:\$PATH"
export PYTHONPATH="$TOOLS/root/usr/lib/python3/dist-packages\${PYTHONPATH:+:\$PYTHONPATH}"
export LD_LIBRARY_PATH="$TOOLS/root/usr/lib/$MULTIARCH\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export BISON_PKGDATADIR="$TOOLS/root/usr/share/bison"
export M4="$TOOLS/root/usr/bin/m4"
ENV
    . "$TOOLS/env.sh"
    m=$(missing)
    [ -n "$m" ] && { echo "still missing: $m"; exit 1; }
    echo "installed into $TOOLS (source $TOOLS/env.sh)"
    ;;
env)
    echo "$TOOLS/env.sh"
    ;;
missing)
    # the missing packages, into /tmp/gramine-vbs-missing (read by gramine-vbs.ps1 setup -Apt)
    missing | tee /tmp/gramine-vbs-missing; echo
    ;;
*)
    sed -n '5,19p' "$0"; exit 2 ;;
esac
