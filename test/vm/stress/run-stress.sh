#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run-stress.sh -- concurrency stress tests of the VM PAL (tier 2 of test/quick.sh). Three plain pthread/futex
# programs, built here and run in a guest with a manifest rendered from manifest.template:
#   hangrepro    thread creation and join, a futex ring, timed futex waits, concurrent thread creation and
#                page-table creation (the hangs fixed in the VM PAL: lost wake-ups, lost timeouts, page tables
#                created concurrently, a thread's stack reused too early)
#   idlebench    idle vCPUs: a futex ring with random sleeps, mmap/munmap churn (TLB shootdowns), short-lived
#                threads and a pinned waiter, with a stall detector
#   madv_stress  madvise(MADV_DONTNEED) of thread stacks and of a large region while other threads map and unmap
# A case passes when the program exits with status 0 and prints its "... OK" line; a hang ends at the timeout.
#
# Usage: test/vm/stress/run-stress.sh [-n ROUNDS] [case...]       (default: every case, one round)
#   cases   hangrepro-join hangrepro-ring hangrepro-twait hangrepro-spawn hangrepro-ptmap idlebench-stress
#           madv-burst madv-region                              (--list prints them with their arguments)
# Environment: SAGUARO_PREFIX (install prefix, default ~/.local/saguaro-vm), SAGUARO_CPU_NUM (default 2),
#   SAGUARO_VM_LAUNCHER (saguaro-vm, or saguaro-sev), STRESS_SCALE (multiplies the duration or the iterations
#   of every case, default 1), TIMEOUT_SCALE (multiplies every timeout, default 1), WORK (build and manifest
#   directory, default <repo>/.stress), LOGDIR (default $WORK/logs).
# Exit status 0 only if every case passed in every round. Needs a vm install (scripts/install.sh --vm) and /dev/kvm.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
ROUNDS=1; CASES=(); LIST=0
while [ $# -gt 0 ]; do
    case $1 in
        -n) ROUNDS=$2; shift ;;
        --list) LIST=1 ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) CASES+=("$1") ;;
    esac
    shift
done
K=${STRESS_SCALE:-1}
# name | program | arguments | timeout (s) | the line that must appear
TABLE="
hangrepro-join|hangrepro|join $((200 * K)) 8|600|JOIN OK
hangrepro-ring|hangrepro|ring $((30 * K))|300|RING OK
hangrepro-twait|hangrepro|twait $((20 * K)) 8|300|TWAIT OK
hangrepro-spawn|hangrepro|spawn 4 16|300|SPAWN OK
hangrepro-ptmap|hangrepro|ptmap $((300 * K)) 8|600|PTMAP OK
idlebench-stress|idlebench|stress $((60 * K)) rtpm|600|STRESS OK
madv-burst|madv_stress|burst 48 32 $((20 * K))|600|BURST OK
madv-region|madv_stress|region 512 $((20 * K)) 4|900|REGION OK
"
if [ $LIST = 1 ]; then echo "$TABLE" | awk -F'|' 'NF==5 {printf "%-18s %s %s\n", $1, $2, $3}'; exit 0; fi
if [ ${#CASES[@]} -eq 0 ]; then mapfile -t CASES < <(echo "$TABLE" | awk -F'|' 'NF==5 {print $1}'); fi

PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
[ -f "$PREFIX/env.sh" ] || { echo "no $PREFIX/env.sh: set SAGUARO_PREFIX to the vm install prefix" >&2; exit 2; }
. "$PREFIX/env.sh"
LAUNCHER=${SAGUARO_VM_LAUNCHER:-saguaro-vm}
command -v "$LAUNCHER" >/dev/null || { echo "$LAUNCHER not on PATH after $PREFIX/env.sh" >&2; exit 2; }
export SAGUARO_CPU_NUM=${SAGUARO_CPU_NUM:-2}
WORK=${WORK:-$REPO/.stress}
LOGDIR=${LOGDIR:-$WORK/logs}
SCALE=${TIMEOUT_SCALE:-1}
ARCH_LIBDIR=/lib/$(gcc -dumpmachine)
mkdir -p "$WORK" "$LOGDIR"

for prog in hangrepro idlebench madv_stress; do
    gcc -Wall -Wextra -O2 -pthread -o "$WORK/$prog" "$HERE/$prog.c" || { echo "build of $prog failed" >&2; exit 2; }
    ( cd "$WORK" && saguaro-manifest -Darch_libdir="$ARCH_LIBDIR" -Dentry="/stress/$prog" -Dstressdir="$WORK" \
          "$HERE/manifest.template" "$prog.manifest" ) 2> "$LOGDIR/manifests.log" \
        || { echo "manifest of $prog failed (see $LOGDIR/manifests.log)" >&2; exit 2; }
done

failed=0
for round in $(seq "$ROUNDS"); do
    for c in "${CASES[@]}"; do
        row=$(echo "$TABLE" | awk -F'|' -v c="$c" 'NF==5 && $1==c')
        [ -n "$row" ] || { echo "unknown case: $c (--list)" >&2; exit 2; }
        IFS='|' read -r _ prog args tmo expect <<<"$row"
        log=$LOGDIR/$c.$round.log
        t0=$SECONDS
        # timeout kills the whole process group at the limit, the launcher and its QEMU
        # shellcheck disable=SC2086
        ( cd "$WORK" && timeout -s KILL $((tmo * SCALE)) "$LAUNCHER" "$prog" $args ) > "$log" 2>&1 < /dev/null
        rc=$?
        if [ $rc -eq 0 ] && grep -q "$expect" "$log" && ! grep -qE '^(HANG|STALL)' "$log"; then
            echo "PASS $c (round $round, $((SECONDS - t0)) s)"
        else
            echo "FAIL $c (round $round, exit status $rc, log $log)"; tail -n 5 "$log"
            failed=1
        fi
    done
done
[ $failed = 0 ] && echo "stress: PASS" || echo "stress: FAIL"
exit $failed
