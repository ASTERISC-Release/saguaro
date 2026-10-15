#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# The Linux side of the Windows-only VBS build (Documentation/devel/vbs-enclave/PLAN-windows-build.md)
# runs in a WSL2 Ubuntu 24.04 aarch64 distribution. This script checks that distribution for the
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
# libraries the packages above need that a minimal Ubuntu may lack (only for the user prefix)
DEPS="pkgconf-bin libpkgconf3"
if [ "$2" = clang ]; then
    PKGS="$PKGS clang:clang"
    DEPS="$DEPS clang-18 libclang-common-18-dev libclang-cpp18 libclang1-18 libllvm18 llvm-18-linker-tools"
fi

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
    [ "$(uname -m)" = aarch64 ] || { echo "not an aarch64 distribution ($(uname -m))"; exit 1; }
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
    for p in $m $DEPS; do dpkg -s "$p" >/dev/null 2>&1 || get="$get $p"; done
    mkdir -p "$TOOLS/debs"
    echo "downloading:$get"
    (cd "$TOOLS/debs" && apt-get download $get)
    for d in "$TOOLS"/debs/*.deb; do dpkg -x "$d" "$TOOLS/root"; done
    cat > "$TOOLS/env.sh" <<ENV
# user-level tools of the vbs build (scripts/vbs/wsl-setup.sh user)
export PATH="$TOOLS/root/usr/bin:\$PATH"
export PYTHONPATH="$TOOLS/root/usr/lib/python3/dist-packages\${PYTHONPATH:+:\$PYTHONPATH}"
export LD_LIBRARY_PATH="$TOOLS/root/usr/lib/aarch64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
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
*)
    sed -n '5,19p' "$0"; exit 2 ;;
esac
