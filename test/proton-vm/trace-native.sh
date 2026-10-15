#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Traces the native Proton runs of the feasibility study (in the container, after
# run-native.sh -p created the prefix and build-tests.sh built the programs): for each program a
# raw `strace -f -ttt -y` trace and a syscall-profile JSON (scripts/syscall-profile.py), into
# $OUT (default .proton-vm/traces). Usage: trace-native.sh [hello|script|hello32|d3d11|vkwin]...
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
. "$HERE/wine-env.sh"
OUT=${OUT:-$ROOT/.proton-vm/traces}
TESTS=${TESTS:-$ROOT/.proton-vm/tests}
mkdir -p "$OUT"
export DISPLAY=${DISPLAY:-:0}
trace() {   # trace NAME CMD...: raw trace, then the profile tool's own run (its strace format)
    local name=$1; shift
    "$PROTON/files/bin/wineserver" -k 2>/dev/null; sleep 1
    echo "=== $name: $*"
    local t0=$(date +%s.%N)
    strace -f -ttt -y -s 128 -o "$OUT/$name.strace" -- "$@" > "$OUT/$name.out" 2> "$OUT/$name.err"
    echo "$name: exit $?, $(python3 -c "import time; print(round(time.time() - $t0, 2))") s wall under strace"
    "$PROTON/files/bin/wineserver" -k 2>/dev/null; sleep 1
    python3 "$ROOT/scripts/syscall-profile.py" profile -o "$OUT/$name.json" -- "$@" > "$OUT/$name.profile.out" 2>&1
    grep -c . "$OUT/$name.strace" | sed "s/^/$name: lines in trace: /"
}
# rss NAME CMD...: runs the program untraced and samples the RSS of every Wine process after 3 s
rss() {
    local name=$1; shift
    "$PROTON/files/bin/wineserver" -k 2>/dev/null; sleep 1
    "$@" > /dev/null 2>&1 &
    sleep 3
    echo "=== $name: RSS after 3 s (KiB, process)"
    ps -eo rss=,comm=,args= | grep -i "wine\|\.exe" | grep -v grep | awk '{print $1, $2, $3}' | sort -rn | tee "$OUT/$name.rss"
    awk '{s+=$1} END {print "total:", s, "KiB in", NR, "processes"}' "$OUT/$name.rss"
    wait
}
[ $# -gt 0 ] || set -- hello script hello32 d3d11 vkwin rss
for what in "$@"; do
    case $what in
        hello)   trace hello $WINE64 "$TESTS/hello.exe" ;;
        script)  WORK=$WORK trace script "$HERE/run-native.sh" -p "$TESTS/hello.exe" ;;
        hello32) WINEARCH=wow64 WINEPREFIX=$WORK-wow64/compatdata/pfx trace hello32 $WINE64 "$TESTS/hello32.exe" ;;   # the new-WoW64 prefix (PROTON_USE_WOW64=1)
        d3d11)   DXVK_FILTER_DEVICE_NAME=Intel trace d3d11 $WINE64 "$TESTS/d3d11.exe" -s 5 ;;
        vkwin)   trace vkwin $WINE64 "$TESTS/vkwin.exe" -s 5 -g 1 ;;
        rss)     rss hello $WINE64 "$TESTS/hello.exe"; DXVK_FILTER_DEVICE_NAME=Intel rss d3d11 $WINE64 "$TESTS/d3d11.exe" -s 6
                 rss vkwin $WINE64 "$TESTS/vkwin.exe" -s 6 -g 1 ;;
    esac
done
