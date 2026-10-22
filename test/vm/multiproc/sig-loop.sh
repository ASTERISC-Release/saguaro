#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Runs the signals probe (sig.sh under bash, multi-process key on) N times back to back and counts
# the runs that miss a line or hang. Run in the container after `scripts/install.sh --vm`.
#
# Usage: test/vm/multiproc/sig-loop.sh [N]   (default 100)
# Environment: SAGUARO_CPU_NUM (default 2), LOAD=<k> runs k busy loops on the host meanwhile,
# RUN_TIMEOUT (seconds per run, default 60), ON_HANG (a shell command run when a guest is still
# running at the timeout, after vm-hang-report.py and before the guest is stopped; the output of
# both goes to $OUT/hang-<i>.txt), WORK (default <repo>/.mp-probes, shared with
# check-probes.sh), OUT (default $WORK/sig-loop/<date-time>), SAGUARO_PREFIX. Each failing run's
# output goes to $OUT/fail-<i>.log.
# Exit status 0 only if every run passed.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
. "$PREFIX/env.sh"
export SAGUARO_CPU_NUM=${SAGUARO_CPU_NUM:-2}
N=${1:-100}
export WORK=${WORK:-$REPO/.mp-probes}
OUT=${OUT:-$WORK/sig-loop/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
# builds the probes and their manifests in $WORK (and checks the probe once)
RERUN=0 "$HERE/check-probes.sh" signals > "$OUT/build.log" 2>&1 || echo "sig-loop: first check failed (see $OUT/build.log); looping anyway"

LOAD_PIDS=()
for _ in $(seq 1 "${LOAD:-0}"); do ( while :; do :; done ) & LOAD_PIDS+=($!); done
trap '[ ${#LOAD_PIDS[@]} = 0 ] || kill "${LOAD_PIDS[@]}" 2>/dev/null' EXIT

fails=0 hangs=0
start=$(date +%s)
cd "$WORK"
for i in $(seq 1 "$N"); do
    # the guest in the background with its QEMU monitor on a socket: a hung one is looked at
    # (vm-hang-report.py, then ON_HANG) and stopped through the monitor, so the launcher reports
    rm -f "$OUT/mon.sock"
    SAGUARO_VM_MONITOR=$OUT/mon.sock setsid saguaro-vm bash /probe/sig.sh > "$OUT/cur.log" 2>&1 < /dev/null &
    pid=$!
    for _ in $(seq 1 $(( ${RUN_TIMEOUT:-60} * 10 ))); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
    hung=0
    if kill -0 $pid 2>/dev/null; then
        hung=1
        { "$HERE/vm-hang-report.py" "$OUT/mon.sock"
          [ -z "${ON_HANG:-}" ] || ( eval "$ON_HANG" )
          "$HERE/vm-hang-report.py" "$OUT/mon.sock" --samples 0 --quit; } > "$OUT/hang-$i.txt" 2>&1
        for _ in $(seq 1 100); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
        kill -KILL -- -$pid 2>/dev/null
    fi
    wait $pid; rc=$?
    out=$(tr -d '\r' < "$OUT/cur.log"; echo "+ exit status $rc"; [ $hung = 0 ] || echo "+ hung")
    ok=1
    for l in "got USR1" "after-self" "child got USR2" "child-status=5" "term-status=143"; do
        grep -qxF -- "$l" <<<"$out" || ok=0
    done
    if [ "$ok" = 0 ]; then
        fails=$((fails + 1))
        [ $hung = 1 ] && hangs=$((hangs + 1))
        printf '%s\n' "$out" > "$OUT/fail-$i.log"
        echo "run $i: FAILED ($OUT/fail-$i.log)"
        [ -n "${MAX_FAILS:-}" ] && [ "$fails" -ge "$MAX_FAILS" ] && { N=$i; break; }
    fi
done
echo "sig-loop: $fails of $N runs failed, $hangs of them hung ($SAGUARO_CPU_NUM vCPUs," \
     "load ${LOAD:-0}, $(( $(date +%s) - start )) s)"
[ "$fails" = 0 ]
