#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# probes.sh -- the multi-process probes of test/vm-multiproc/ (spawn.c, forks.c, sig.sh and
# manifest.template, sys.experimental__multiprocess on) under the AArch64 gramine-vm. The
# x86_64 runner (test/vm-multiproc/run-probes.sh) hardcodes the x86_64 library directory; this one
# takes the build machine's (gcc -dumpmachine) and keeps one log per probe.
#
# Usage: test/vm-multiproc-aarch64/probes.sh [--prefix DIR] [--cpus N]
#            [spawn|sh|forks|signals|forks-mem|eintr|efault]...   (default: sh spawn)
# eintr: test/vm-multiproc-aarch64/eintr.c, a signal against each kind of blocking call.
# efault: test/vm-multiproc-aarch64/efault.c, LibOS faults on application memory, key on and off.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
CPUS=2
WHAT=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --cpus) CPUS=$2; shift 2 ;;
        *) WHAT+=("$1"); shift ;;
    esac
done
[ ${#WHAT[@]} -gt 0 ] || WHAT=(sh spawn)
export GRAMINE_PREFIX=$PREFIX
. "$PREFIX/env.sh"
WORK=${WORK:-$REPO/.mp-probes-aarch64}
LOGS=${LOGS:-$WORK/logs}
mkdir -p "$WORK" "$LOGS" && cd "$WORK"
SRC=$REPO/test/vm-multiproc
cp "$SRC"/spawn.c "$SRC"/forks.c "$SRC"/sig.sh "$SRC"/manifest.template "$HERE"/eintr.c "$HERE"/efault.c .
gcc -O2 -o spawn spawn.c && gcc -O2 -o forks forks.c && gcc -O2 -o eintr eintr.c -lpthread && \
    gcc -O2 -o efault efault.c || exit 2
LIBDIR=/lib/$(gcc -dumpmachine)
for entry in /usr/bin/sh /usr/bin/bash "$WORK/spawn" "$WORK/forks" "$WORK/eintr" "$WORK/efault"; do
    name=$(basename "$entry")
    case $entry in "$WORK"/*) e=/probe/$name ;; *) e=$entry ;; esac
    gramine-manifest -Darch_libdir="$LIBDIR" -Dentry="$e" -Dprobedir="$WORK" \
        manifest.template "$name.manifest" 2>/dev/null || exit 2
done
run() {
    local tag=$1 t=$2; shift 2
    local load; load=$(cut -d' ' -f1 /proc/loadavg)
    local start; start=$(date +%s%N)
    GRAMINE_CPU_NUM=$CPUS GRAMINE_VM_SERIAL=$LOGS/$tag.serial timeout -s KILL "$t" "$@" \
        > "$LOGS/$tag.log" 2>&1
    local rc=$?
    echo "== $tag: rc=$rc time=$(( ($(date +%s%N) - start) / 1000000 ))ms vcpus=$CPUS load1=$load"
    grep -v "self-check\|Frame-pool\|frame pool\|vm-relay\|work in progress\|Address-space switch\|child exited" \
        "$LOGS/$tag.log" | head -${LINES_SHOWN:-20}
}
for what in "${WHAT[@]}"; do
    case $what in
        sh)      run sh 60 gramine-vm sh -c 'echo hi | cat; ls /probe | wc -l' ;;
        spawn)   run spawn 60 gramine-vm spawn ;;
        forks)   run forks-1000 900 gramine-vm forks 1000 1; run forks-64x8 600 gramine-vm forks 64 8 ;;
        signals) run signals 60 gramine-vm bash /probe/sig.sh ;;
        eintr)   # one guest per case: a case that times out ends its process
                 for c in poll ppoll-null pause pipe-read epoll_wait select nanosleep futex \
                          child-pause; do
                     LINES_SHOWN=${LINES_SHOWN:-2} run eintr-$c 40 gramine-vm eintr $c
                 done ;;
        efault)  # key on: each case in a child; key off: in the process itself (no fork)
                 grep -v experimental__multiprocess efault.manifest > efault-nokey.manifest
                 ln -sf efault efault-nokey
                 for c in sigframe altstack; do
                     for k in on off; do
                         m=efault; [ $k = off ] && m=efault-nokey
                         LINES_SHOWN=0 run efault-$c-$k 60 gramine-vm $m $c
                         grep -a "child \|no fork\|Internal\|Panic\|exited with" \
                             "$LOGS/efault-$c-$k.log" | tail -4
                     done
                 done ;;
        forks-mem)
            # pool frames in use after each child's exit (log_debug of pal_process.c): the first
            # and the last of N sequential forks, and of N forks in batches of 8
            sed 's/^\(loader\.\)\{0,1\}log_level = .*/\1log_level = "debug"/' forks.manifest > forks-dbg.manifest
            ln -sf forks forks-dbg
            for spec in "${FORKS_N:-1000} 1" "${FORKS_P:-64} 8"; do
                set -- $spec
                LINES_SHOWN=0 run forks-mem-$1x$2 900 gramine-vm forks-dbg $1 $2
                grep -a "forks=" "$LOGS/forks-mem-$1x$2.log"
                grep -ao "process exit: [0-9]* frame-pool frames in use" "$LOGS/forks-mem-$1x$2.log" \
                    | awk '{print $3}' | sed -n '1p;$p' | tr '\n' ' ' \
                    | awk '{print "frames in use after the first child: " $1 ", after the last: " $2 " (growth " $2 - $1 ")"}'
            done ;;
    esac
done
