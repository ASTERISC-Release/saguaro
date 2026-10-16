#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Runs an installed Windows game natively through GE-Proton's Wine (the prefix of run-native.sh -p)
# on a virtual X server (Xvfb), so that nothing appears on the real screen, and records what the
# game-intake step of Documentation/devel/proton-vm/PLAN.md asks for: whether it starts, the
# processes and threads, the resident memory, a screenshot every few seconds, and the Wine
# output. Usage (in the container):
#   run-game.sh [-d :N] [-s SECONDS] [-o OUTDIR] [-g WxH] EXE [ARGS...]
# EXE is a Unix path; the game runs with its folder as the working directory. Defaults: display
# :7 (an Xvfb of 1280x720x24 is started if the display is not up), 90 s, OUTDIR
# .proton-vm/games/runs/<exe name>-<time>. Environment as run-native.sh (WINEDEBUG, WINEDLLOVERRIDES,
# WINEESYNC...); XDO=... runs `xdotool` commands (a file of lines "sleep N" / "key X" / "mousemove X Y"
# / "click 1") against the display while the game runs. SHOT_EVERY (default 10) seconds between screenshots.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DISP=:7; SECS=90; OUT=; GEOM=1280x720
while getopts "d:s:o:g:" o; do case $o in d) DISP=$OPTARG ;; s) SECS=$OPTARG ;; o) OUT=$OPTARG ;; g) GEOM=$OPTARG ;; esac; done
shift $((OPTIND - 1))
EXE=$1; shift
NAME=$(basename "$EXE" .exe)
[ -n "$OUT" ] || OUT=$ROOT/.proton-vm/games/runs/$NAME-$(date -u +%H%M%S)
mkdir -p "$OUT"
export DISPLAY=$DISP
if ! xdpyinfo >/dev/null 2>&1; then
    Xvfb "$DISP" -screen 0 "${GEOM}x24" -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
    for _ in $(seq 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
fi
echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) display $DISP ($GEOM) exe $EXE args: $* secs $SECS out $OUT $(uptime | sed 's/.*load/load/')" | tee "$OUT/run.txt"
cd "$(dirname "$EXE")" || exit 1
START=$(date +%s)
# the study's way to start the 64-bit Wine (test/proton-vm/wine-env.sh): files/bin/wine would start the
# 32-bit loader first, which this container cannot run
. "$HERE/wine-env.sh"
$WINE64 "$(basename "$EXE")" "$@" >"$OUT/wine.out" 2>"$OUT/wine.err" &
WPID=$!
# a Windows-style line for xdotool scripts
if [ -n "${XDO:-}" ]; then
    ( while read -r cmd arg1 arg2; do
          case $cmd in sleep) sleep "$arg1" ;; "") ;; \#*) ;; *) xdotool "$cmd" $arg1 $arg2 ;; esac
      done <"$XDO" ) &
fi
I=0
while [ $(( $(date +%s) - START )) -lt "$SECS" ]; do
    sleep "${SHOT_EVERY:-10}"
    I=$((I + 1))
    T=$(( $(date +%s) - START ))
    xwd -root -silent 2>/dev/null | convert xwd:- "$OUT/shot-$(printf %02d $I).png" 2>/dev/null
    WINS=$(xdotool search --onlyvisible --name . 2>/dev/null | while read -r w; do xdotool getwindowname "$w" 2>/dev/null; done | tr '\n' '|')
    PROCS=$(ps -eo pid,nlwp,rss,comm,args 2>/dev/null | grep -E "wine|\.exe" | grep -v -E "grep|run-game|run-native")
    NP=$(echo "$PROCS" | grep -c .); NT=$(echo "$PROCS" | awk '{t+=$2} END {print t+0}'); RSS=$(echo "$PROCS" | awk '{t+=$3} END {printf "%.0f", t/1024}')
    printf "t=%ss procs=%s threads=%s rss=%sMiB windows=%s\n" "$T" "$NP" "$NT" "$RSS" "$WINS" | tee -a "$OUT/run.txt"
    kill -0 $WPID 2>/dev/null || { echo "# wine exited after $T s" | tee -a "$OUT/run.txt"; break; }
done
ps -eo pid,ppid,nlwp,rss,etimes,comm,args | grep -E "wine|\.exe" | grep -v -E "grep|run-game|run-native" >"$OUT/ps.txt"
# the final state, then stop everything of this prefix
xwd -root -silent 2>/dev/null | convert xwd:- "$OUT/shot-final.png" 2>/dev/null
"$PROTON/files/bin/wineserver" -k 2>/dev/null
wait $WPID; echo "# wine exit $? (after $(( $(date +%s) - START )) s)" | tee -a "$OUT/run.txt"
echo "# output: $OUT (wine.out, wine.err, run.txt, ps.txt, shot-*.png)"
