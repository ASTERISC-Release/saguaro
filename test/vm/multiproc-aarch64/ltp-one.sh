#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# ltp-one.sh -- one LTP test of a harness directory made by ltp-groups.sh, under gramine-vm, with
# an optional log level, keeping the console and serial logs (for debugging one failure).
# Usage: test/vm/multiproc-aarch64/ltp-one.sh --prefix DIR --tag NAME [--log LEVEL] [--cpus N]
#            [--monitor SOCKET] test [args...]
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PREFIX=; TAG=; LEVEL=; CPUS=1; MON=
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --log) LEVEL=$2; shift 2 ;;
        --cpus) CPUS=$2; shift 2 ;;
        --monitor) MON=$2; shift 2 ;;
        *) break ;;
    esac
done
T=$1; shift
. "$PREFIX/env.sh"
export GRAMINE_PREFIX=$PREFIX
H=$REPO/.ltp-harness-$TAG
cd "$H/install/testcases/bin" 2>/dev/null || cd "$H"
OUT=$REPO/.ltp-results/one
mkdir -p "$OUT"
app=$T
if [ -n "$LEVEL" ]; then
    sed "s/^log_level = .*/log_level = \"$LEVEL\"/" "$H/$T.manifest" > "$H/$T-dbg.manifest"
    grep -q '^log_level' "$H/$T-dbg.manifest" || \
        sed -i "0,/^\[loader\]/s//[loader]\nlog_level = \"$LEVEL\"/" "$H/$T-dbg.manifest"
    app=$T-dbg
fi
cd "$H"
[ -n "$MON" ] && export GRAMINE_VM_MONITOR=$MON
GRAMINE_CPU_NUM=$CPUS GRAMINE_VM_SERIAL=$OUT/$T.serial \
    timeout -s KILL ${RUN_TIMEOUT:-120} gramine-vm $app "$@" > "$OUT/$T.log" 2>&1
echo "rc=$? log=$OUT/$T.log"
