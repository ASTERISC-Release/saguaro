#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# The runtime of a VBS enclave image, on an aarch64 or x86_64 Linux machine (WSL, the build container): the
# vbs glibc (scripts/vbs/build-glibc.sh, into build-vbs-glibc) and the meson build of the vbs PAL
# and LibOS (build-vbs) when missing, then ninja. Used by wsl-build.sh (saguaro-vbs.ps1 on
# Windows) and by `saguaro-vbs runtime` (the make VBS=1 flow). Step logs: build-vbs-logs/.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOLS=${SAGUARO_VBS_TOOLS:-$HOME/.local/share/saguaro-vbs/tools}
[ -f "$TOOLS/env.sh" ] && . "$TOOLS/env.sh"
PREFIX=${SAGUARO_VBS_LOG_PREFIX:-[vbs-runtime]}

log() { echo "$PREFIX $*"; }

LOGS=$ROOT/build-vbs-logs
mkdir -p "$LOGS"
timed() {  # name command...: run a step with its output in a log, print its time
    _n=$1; shift
    _s=$(date +%s)
    "$@" >"$LOGS/$_n.log" 2>&1 || { tail -30 "$LOGS/$_n.log"; log "$_n failed ($LOGS/$_n.log)"; exit 1; }
    log "$_n: $(( $(date +%s) - _s )) s"
}

cd "$ROOT"
if [ ! -f build-vbs-glibc/usr/lib/libc.so.6 ]; then
    log "first build: the vbs glibc (about 2-3 minutes)"
    [ -f subprojects/packagecache/glibc-2.39.tar.gz ] \
        || timed glibc-download meson subprojects download glibc-2.39-1
    timed glibc sh scripts/vbs/build-glibc.sh
    rm -rf build-vbs-glibc.build
fi
if [ "$(uname -m)" != x86_64 ] && [ ! -f build-vbs-glibc/usr/lib/libsoftfp-x18.a ]; then
    # a sysroot from before build-glibc.sh installed the x18-free soft-float routines
    timed softfp sh scripts/vbs/build-glibc.sh --softfp
fi
# gcc 14 and later make incompatible pointer types an error, which a few LibOS sources still have
# (gcc 15 of Ubuntu 26.04, the x64 machine's WSL); they stay warnings there
CARGS=
[ "$(gcc -dumpversion | cut -d. -f1)" -ge 14 ] 2>/dev/null \
    && CARGS=-Dc_args=-Wno-error=incompatible-pointer-types
if [ ! -f build-vbs/build.ninja ]; then
    log "first build: meson setup of the vbs PAL and LibOS"
    timed meson-setup meson setup build-vbs -Ddirect=disabled -Dvbs=enabled -Dlibc=none \
        -Dtests=disabled -Dbuildtype=release -Dgraphics=disabled $CARGS
fi
timed ninja ninja -C build-vbs
