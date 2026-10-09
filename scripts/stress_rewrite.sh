#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# stress_rewrite.sh — repeat the rewrite_stress scenarios many times (the interesting races only
# happen on cold processes, so each run is a fresh gramine-direct).
#
# Usage:
#   scripts/stress_rewrite.sh [-n RUNS] [--rewrite on|off] [scenario...]
#     -n RUNS            cold runs per scenario (default 40)
#     --rewrite on|off   force sys.rewrite_syscalls (rebuilds the test manifest; default: LibOS default)
#   Scenarios: threads fork exec signals signals_timer signals_kill restart blob jit mprotect
#   threadstorm (default: all of them; the two signals_* variants are subsets of `signals`).
#   TMO=<s> kills a hung run (default 30).
# Failing runs' output is kept in $FAILDIR (default /tmp/rewrite-stress-fail).
# Needs the install from ./install.sh and `gramine-test build` prerequisites (python deps).
set -uo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
DIR=$HERE/../libos/test/regression
. "$HERE/gramine-env.sh"   # GRAMINE_PREFIX, PATH, PYTHONPATH
FAILDIR=${FAILDIR:-/tmp/rewrite-stress-fail}
N=40; MODE=""; SCEN=(); TMO=${TMO:-30}
while [ $# -gt 0 ]; do
    case "$1" in
        -n) N=$2; shift 2 ;;
        --rewrite) MODE=$2; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) SCEN+=("$1"); shift ;;
    esac
done
[ ${#SCEN[@]} -gt 0 ] || SCEN=(threads fork exec signals signals_timer signals_kill restart blob jit mprotect threadstorm)

cd "$DIR" || exit 1
case "$MODE" in
    on)  export REWRITE=true ;;
    off) export REWRITE=false ;;
    "")  unset REWRITE ;;
    *)   echo "--rewrite must be on|off"; exit 2 ;;
esac
rm -f rewrite_stress.manifest
gramine-test build >/dev/null 2>&1 || { echo "gramine-test build failed"; exit 1; }

mkdir -p "$FAILDIR"; rm -f "$FAILDIR"/*
total_bad=0
for sc in "${SCEN[@]}"; do
    ok=0; bad=0; hang=0
    for _ in $(seq 1 "$N"); do
        out=$(mktemp)
        timeout -s KILL "$TMO" gramine-direct rewrite_stress "$sc" >"$out" 2>&1
        rc=$?
        if [ $rc -eq 0 ] && grep -q "^OK $sc" "$out"; then
            ok=$((ok+1)); rm -f "$out"
        elif [ $rc -eq 137 ]; then
            hang=$((hang+1)); mv "$out" "$FAILDIR/$sc-hang$hang.out"
        else
            bad=$((bad+1)); mv "$out" "$FAILDIR/$sc-fail$bad.out"
        fi
        pkill -KILL -x loader 2>/dev/null   # reap anything a hung run left behind
    done
    printf '%-14s ok=%d fail=%d hang=%d\n' "$sc" "$ok" "$bad" "$hang"
    total_bad=$((total_bad+bad+hang))
done
[ "$total_bad" -eq 0 ] && echo "ALL PASSED (rewrite=${MODE:-default})" || echo "FAILURES: $total_bad (see $FAILDIR)"
exit $((total_bad != 0))
