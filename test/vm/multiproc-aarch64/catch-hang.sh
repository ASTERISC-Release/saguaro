#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# catch-hang.sh -- run one LTP test (ltp-one.sh) up to N times and, the moment its console shows
# PATTERN, sample every vCPU with vm-hang-report.py (for stalls that do not last).
# Usage: test/vm-multiproc-aarch64/catch-hang.sh --prefix DIR --tag NAME --cpus N --runs R
#            --pattern REGEX test
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PREFIX=; TAG=; CPUS=4; RUNS=10; PAT=shootdown
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --cpus) CPUS=$2; shift 2 ;;
        --runs) RUNS=$2; shift 2 ;;
        --pattern) PAT=$2; shift 2 ;;
        *) break ;;
    esac
done
T=$1
LOG=$REPO/.ltp-results/one/$T.log
for i in $(seq "$RUNS"); do
    rm -f "$LOG" /tmp/mon-catch
    RUN_TIMEOUT=${RUN_TIMEOUT:-90} "$HERE/ltp-one.sh" --prefix "$PREFIX" --tag "$TAG" --cpus "$CPUS" \
        --monitor /tmp/mon-catch "$T" > /dev/null 2>&1 &
    pid=$!
    caught=0
    while kill -0 $pid 2>/dev/null; do
        if [ -f "$LOG" ] && grep -qE "$PAT" "$LOG"; then
            echo "run $i: caught '$PAT'"
            GRAMINE_PREFIX=$PREFIX python3 "$HERE/vm-hang-report.py" /tmp/mon-catch --samples 4
            grep -E "$PAT" "$LOG" | head -6
            caught=1
            break
        fi
        sleep 0.2
    done
    wait $pid
    [ $caught = 1 ] && exit 0
    echo "run $i: not seen ($(grep -c 'exited with code' "$LOG" 2>/dev/null) exits)"
done
exit 1
