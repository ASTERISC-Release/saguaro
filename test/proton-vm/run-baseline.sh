#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Game intake on the real display (PLAN.md step 0, run from the host, not the container): runs a
# game through run-game.sh on Xwayland :0 natively or under gramine-vm with the Hyprland rule that
# keeps the window floating and unfocused (class steam_proton, removed afterwards), captures the
# window with grim every 20 s and records the load with every capture (TESTING.md "Window size").
# Usage: run-baseline.sh NAME EXE SECS [native|vm]   (EXE relative to the checkout, e.g.
# .proton-vm/games/doom/DOOM.exe; EXTRA_ARGS: the game's arguments; the vm loader uses GPU=1,
# NET=deny, 4 vCPUs and 12 GiB; WOW64=1 for a 32-bit game, as wine-env.sh). Output:
# .proton-vm/games/runs/NAME (run-game.sh's files, plus host.txt and host-T.png).
set -u
NAME=$1; EXE=$2; SECS=$3; LOADER=${4:-native}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); cd "$ROOT" || exit 1
O=.proton-vm/games/runs/$NAME; mkdir -p "$O"
hyprctl eval 'hl.window_rule({ match = { class = "^(steam_proton)$" }, float = true, no_focus = true, size = { 1280, 720 } })' >/dev/null
echo "# $NAME $LOADER :0 $(uptime | sed 's/.*load/load/')" > "$O/host.txt"
( S0=$(date +%s); for T in $(seq 20 20 $((SECS - 5))); do
    while [ $(( $(date +%s) - S0 )) -lt "$T" ]; do sleep 1; done
    hyprctl clients -j | jq -r '.[] | select(.class=="steam_proton") | "t='"$T"' \(.title) floating=\(.floating) at=\(.at) size=\(.size) load='"$(cut -d' ' -f1 /proc/loadavg)"'"' >> "$O/host.txt"
    G=$(hyprctl clients -j | jq -r '.[] | select(.class=="steam_proton") | "\(.at[0]),\(.at[1]) \(.size[0])x\(.size[1])"' | tail -1)
    [ -n "$G" ] && grim -g "$G" "$O/host-$T.png"
  done ) &
if [ "$LOADER" = native ]; then
    container/run.sh "WOW64=${WOW64:-0} DXVK_HUD=fps WINEDEBUG=err+all timeout $((SECS + 60)) test/proton-vm/run-game.sh -d :0 -s $SECS -o \$PWD/$O \$PWD/$EXE ${EXTRA_ARGS:-} >/dev/null 2>&1; tail -1 \$PWD/$O/run.txt"
else
    container/run.sh "WOW64=${WOW64:-0} X_TARGET=:0 GPU=1 NET=deny DXVK_HUD=fps EGL_VULKAN_REFRESH=1000 EGL_VULKAN_RATE_LOG=10 SAGUARO_CPU_NUM=4 SAGUARO_RAM_SIZE=12G WINEDEBUG=err+all SAGUARO_PREFIX=/root/.local/saguaro-vm timeout $((SECS + 100)) test/proton-vm/run-game.sh -l vm -d :1 -s $SECS -o \$PWD/$O \$PWD/$EXE ${EXTRA_ARGS:-} >/dev/null 2>&1; tail -1 \$PWD/$O/run.txt"
fi
wait; hyprctl reload config-only >/dev/null
echo "windows left: $(hyprctl clients -j | jq -r '.[].class' | grep -c steam_proton)"
cat "$O/host.txt"; grep "windows=" "$O/run.txt" | tail -2 | cut -c1-120
