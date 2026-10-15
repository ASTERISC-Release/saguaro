#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Multi-process probes for gramine-vm with sys.experimental__multiprocess (run in the container,
# after `scripts/install.sh --vm`): spawn (sh -c, system(), posix_spawn), signals between
# processes (bash), fork stress, then nginx with workers and redis BGSAVE (both use host ports
# 8002/8444 and 6379: check that nobody else uses them), and `misc`: the dev-libos-misc calls
# (SysV IPC, memfd, file links and times, timers, inotify) used from a forked child.
# Usage: test/vm-multiproc/run-probes.sh [spawn|signals|forks|nginx|redis|misc]...
#        (no argument: spawn signals forks)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd); export REPO
WORK=${WORK:-$REPO/.mp-probes}
. ~/.local/gramine-vm/env.sh
mkdir -p "$WORK" && cd "$WORK"
cp "$HERE"/spawn.c "$HERE"/forks.c "$HERE"/misc-fork.c "$HERE"/sig.sh "$HERE"/manifest.template "$HERE"/misc-fork.manifest.template .
gcc -O2 -o spawn spawn.c && gcc -O2 -o forks forks.c && gcc -O2 -o misc-fork misc-fork.c
for entry in /usr/bin/sh /usr/bin/bash "$WORK/spawn" "$WORK/forks" "$WORK/misc-fork"; do
    name=$(basename "$entry")
    case $entry in "$WORK"/*) e=/probe/$name ;; *) e=$entry ;; esac
    template=manifest.template; [ "$name" = misc-fork ] && template=misc-fork.manifest.template
    gramine-manifest -Darch_libdir=/lib/x86_64-linux-gnu -Dentry="$e" -Dprobedir="$WORK" \
        "$template" "$name.manifest" 2>/dev/null
done
run() { GRAMINE_CPU_NUM=${CPUS:-2} timeout -s KILL "$@" 2>&1 | grep -v "self-check\|Frame-pool\|boot\]\|Physical\|Configuring\|REM_MEM\|vm-relay\|frame pool\|child exited"; }
[ $# -gt 0 ] || set -- spawn signals forks
for what in "$@"; do
    case $what in
        spawn)   run 60 gramine-vm sh -c 'echo hi | cat; ls /probe | wc -l'; run 60 gramine-vm spawn ;;
        signals) run 60 gramine-vm bash /probe/sig.sh ;;
        forks)   run 600 gramine-vm forks 1000 1; run 600 gramine-vm forks 64 8 ;;
        nginx)   "$HERE"/nginx-workers.sh ;;
        redis)   "$HERE"/redis-bgsave.sh ;;
        misc)    mkdir -p "$WORK/scratch" "$WORK/tmp"; rm -f "$WORK"/scratch/mf-* "$WORK"/tmp/.gramine-*
                 run 120 gramine-vm misc-fork /probe/scratch ;;
    esac
done
