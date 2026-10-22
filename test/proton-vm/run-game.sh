#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Runs an installed Windows game natively through GE-Proton's Wine (the prefix of run-native.sh -p)
# on a virtual X server (Xvfb), so that nothing appears on the real screen, and records what the
# game-intake step of dev docs: proton-vm/PLAN.md asks for: whether it starts, the
# processes and threads, the resident memory, a screenshot every few seconds, and the Wine
# output. Usage (in the container):
#   run-game.sh [-l native|vm] [-d :N] [-s SECONDS] [-o OUTDIR] [-g WxH] EXE [ARGS...]
# -l vm runs the game under saguaro-vm through run-wine.sh (SAGUARO_CPU_NUM, SAGUARO_RAM_SIZE,
# WINEDEBUG... as there; the guest reaches the display through saguaro-vm's vsock relay).
# EXE is a Unix path; the game runs with its folder as the working directory. Defaults: display
# :7 (an Xvfb of 1280x720x24 is started if the display is not up), 90 s, OUTDIR
# .proton-vm/games/runs/<exe name>-<time>. Environment as run-native.sh (WINEDEBUG, WINEDLLOVERRIDES,
# WINEESYNC...); XDO=... runs `xdotool` commands (a file of lines "sleep N" / "key X" / "mousemove X Y"
# / "click 1") against the display while the game runs. SHOT_EVERY (default 10) seconds between screenshots.
# TIMESTAMP=1 stamps every console line of a -l vm run with seconds since the start (ms resolution).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DISP=:7; SECS=90; OUT=; GEOM=1280x720; LOADER=native
while getopts "l:d:s:o:g:" o; do case $o in l) LOADER=$OPTARG ;; d) DISP=$OPTARG ;; s) SECS=$OPTARG ;; o) OUT=$OPTARG ;; g) GEOM=$OPTARG ;; esac; done
shift $((OPTIND - 1))
EXE=$1; shift
NAME=$(basename "$EXE" .exe)
[ -n "$OUT" ] || OUT=$ROOT/.proton-vm/games/runs/$NAME-$LOADER-$(date -u +%H%M%S)
mkdir -p "$OUT"
[ -z "${XDO:-}" ] || XDO=$(readlink -f "$XDO") # the run changes into the game's folder below
# X_TARGET=:M shows the guest's display :N (-d) on the host server :M instead: the relay of vsock
# port 6000+N then connects to :M, and screenshots and window checks use :M (for the real display
# when another relay already owns port 6000, FINDINGS.md 2026-10-16)
TARGET=${X_TARGET:-$DISP}
export DISPLAY=$TARGET
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
if [ "$LOADER" = vm ]; then
    # saguaro-vm relays vsock port 6000+N to /tmp/.X11-unix/XN; in the container that directory is
    # the host's (not writable), so Xvfb/Xorg listen on the abstract socket only: relay to that one
    N=${DISP#:}
    if [ ! -S "/tmp/.X11-unix/X$N" ] || [ "$TARGET" != "$DISP" ]; then
        # (setsid -f: detached, or the script's final wait would block on it; the relay stays for
        # the next runs of the same display, like a relay started by hand)
        if ! ss -H -ln --vsock "sport = :$((6000 + N))" 2>/dev/null | grep -q .; then
            if [ -S "/tmp/.X11-unix/X${TARGET#:}" ]; then
                # a host server with a filesystem socket (Xwayland): through the filtering relay,
                # which hides DRI3 and Present from the guest (tools/saguaro-vm-xfilter.py; a DRI3
                # server makes Mesa open the GPU through the X connection with fd passing, which no
                # vsock relay can carry: the guest hangs in winex11's GL initialisation)
                setsid -f nohup python3 "$ROOT/tools/saguaro-vm-xfilter.py" --port $((6000 + N)) \
                    --x-socket "/tmp/.X11-unix/X${TARGET#:}" </dev/null >"$OUT/socat.log" 2>&1
            else
                setsid -f nohup socat -b 1048576 "VSOCK-LISTEN:$((6000 + N)),fork,reuseaddr,max-children=50" \
                    "ABSTRACT-CONNECT:/tmp/.X11-unix/X${TARGET#:}" </dev/null >"$OUT/socat.log" 2>&1
            fi
        fi
        export SAGUARO_VM_X_PROXY=0
        sleep 0.5
    fi
    # the X relay without the GEM arenas unless GPU=1 (run-wine.sh turns them on for the GPU)
    [ "${GPU:-0}" = 1 ] || export SAGUARO_VM_GRAPHICS=${SAGUARO_VM_GRAPHICS:-0}
    if [ "${TIMESTAMP:-0}" = 1 ]; then
        # each console line stamped with seconds since the start on arrival (the wineserver's
        # +server trace has no timestamps of its own); ms resolution, arrival order kept
        GAME=$(dirname "$EXE") DISPLAY_NUM=${DISP#:} TIMEOUT=$((SECS + 60)) "$HERE/run-wine.sh" vm "$EXE" "$@" 2>"$OUT/wine.err" | \
            python3 -u -c 'import sys,time
t0=time.monotonic()
for l in sys.stdin:
    sys.stdout.write("[+%08.3f] %s" % (time.monotonic()-t0, l))' >"$OUT/wine.out" &
    else
        GAME=$(dirname "$EXE") DISPLAY_NUM=${DISP#:} TIMEOUT=$((SECS + 60)) "$HERE/run-wine.sh" vm "$EXE" "$@" >"$OUT/wine.out" 2>"$OUT/wine.err" &
    fi
else
    $WINE64 "$(basename "$EXE")" "$@" >"$OUT/wine.out" 2>"$OUT/wine.err" &
fi
WPID=$!
# a Windows-style line for xdotool scripts
if [ -n "${XDO:-}" ]; then
    # "focus NAME" names the window every later key/click goes to: it is focused again right
    # before each of them (the newest window of that name: a game recreates its window when it
    # switches to full screen, and nothing else gives focus on the virtual server)
    ( FOCUS=
      refocus() { [ -n "$FOCUS" ] || return 0; W=$(xdotool search --name "$FOCUS" 2>/dev/null | tail -1)
                  [ -n "$W" ] && xdotool windowfocus --sync "$W"
                  echo "$(date +%T) focus '$FOCUS' -> window ${W:-none}, now $(xdotool getwindowfocus 2>/dev/null)" >>"$OUT/xdo.log"; }
      while read -r cmd arg1 arg2; do
          case $cmd in
              sleep) sleep "$arg1" ;;
              focus) FOCUS=$arg1; refocus ;;
              key|type|click|mousemove) refocus; xdotool "$cmd" $arg1 $arg2; echo "$(date +%T) $cmd $arg1 $arg2: exit $?" >>"$OUT/xdo.log" ;;
              "") ;; \#*) ;;
              *) xdotool "$cmd" $arg1 $arg2; echo "$(date +%T) $cmd $arg1 $arg2: exit $?" >>"$OUT/xdo.log" ;;
          esac
      done <"$XDO" ) >>"$OUT/xdo.log" 2>&1 &
fi
I=0
while [ $(( $(date +%s) - START )) -lt "$SECS" ]; do
    sleep "${SHOT_EVERY:-10}"
    I=$((I + 1))
    T=$(( $(date +%s) - START ))
    xwd -root -silent 2>/dev/null | convert xwd:- "$OUT/shot-$(printf %02d $I).png" 2>/dev/null
    WINS=$(xdotool search --onlyvisible --name . 2>/dev/null | while read -r w; do xdotool getwindowname "$w" 2>/dev/null; done | tr '\n' '|')
    PROCS=$(ps -eo pid,nlwp,rss,comm,args 2>/dev/null | grep -E "wine|\.exe|qemu-system" | grep -v -E "grep|run-game|run-native|run-wine")
    NP=$(echo "$PROCS" | grep -c .); NT=$(echo "$PROCS" | awk '{t+=$2} END {print t+0}'); RSS=$(echo "$PROCS" | awk '{t+=$3} END {printf "%.0f", t/1024}')
    printf "t=%ss procs=%s threads=%s rss=%sMiB windows=%s\n" "$T" "$NP" "$NT" "$RSS" "$WINS" | tee -a "$OUT/run.txt"
    kill -0 $WPID 2>/dev/null || { echo "# wine exited after $T s" | tee -a "$OUT/run.txt"; break; }
done
ps -eo pid,ppid,nlwp,rss,etimes,comm,args | grep -E "wine|\.exe|qemu-system" | grep -v -E "grep|run-game|run-native|run-wine" >"$OUT/ps.txt"
# the final state, then stop everything of this prefix
xwd -root -silent 2>/dev/null | convert xwd:- "$OUT/shot-final.png" 2>/dev/null
if [ "$LOADER" = vm ]; then
    # the guest ends with its first process or at run-wine.sh's timeout; stop it now
    pkill -TERM -f "qemu-system-x86_64" 2>/dev/null
else
    "$PROTON/files/bin/wineserver" -k 2>/dev/null
fi
wait $WPID; echo "# wine exit $? (after $(( $(date +%s) - START )) s)" | tee -a "$OUT/run.txt"
echo "# output: $OUT (wine.out, wine.err, run.txt, ps.txt, shot-*.png)"
