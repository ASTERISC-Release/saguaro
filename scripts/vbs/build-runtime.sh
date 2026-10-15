#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# The runtime of a VBS enclave image, on an aarch64 Linux machine (WSL, the build container): the
# vbs glibc (scripts/vbs/build-glibc.sh, into build-vbs-glibc) and the meson build of the vbs PAL
# and LibOS (build-vbs) when missing, then ninja. Used by wsl-build.sh (gramine-vbs.ps1 on
# Windows) and by `gramine-vbs runtime` (the make VBS=1 flow). Step logs: build-vbs-logs/.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOLS=${GRAMINE_VBS_TOOLS:-$HOME/.local/share/gramine-vbs/tools}
[ -f "$TOOLS/env.sh" ] && . "$TOOLS/env.sh"
PREFIX=${GRAMINE_VBS_LOG_PREFIX:-[vbs-runtime]}

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
if [ ! -f build-vbs-glibc/usr/lib/libsoftfp-x18.a ]; then
    # a sysroot from before build-glibc.sh installed the x18-free soft-float routines
    timed softfp sh scripts/vbs/build-glibc.sh --softfp
fi
if [ ! -f build-vbs/build.ninja ]; then
    log "first build: meson setup of the vbs PAL and LibOS"
    timed meson-setup meson setup build-vbs -Ddirect=disabled -Dvbs=enabled -Dlibc=none \
        -Dtests=disabled -Dbuildtype=release -Dgraphics=disabled
fi
timed ninja ninja -C build-vbs
