#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Frame rate of vkcube without its start-up cost: runs it for N1 and N2 frames (--c) and reports
# (N2 - N1) / (t2 - t1). Usage: vkcube-rate.sh GPU_NUMBER N1 N2 [launcher...]
# e.g.  vkcube-rate.sh 1 1000 5000                         (native)
#       vkcube-rate.sh 1 1000 5000 saguaro-direct direct-vkcube
# Window 1920x1080, immediate present mode (no vsync). Prints one line with both times and the rate.
set -u
gpu=$1 n1=$2 n2=$3
shift 3
launcher=("$@")
[ ${#launcher[@]} -eq 0 ] && launcher=(vkcube)
run() {
    local s e
    s=$(date +%s.%N)
    timeout 300 "${launcher[@]}" --gpu_number "$gpu" --present_mode 0 --width 1920 --height 1080 \
        --c "$1" >/dev/null 2>&1 || echo "rc=$? for $1 frames" >&2
    e=$(date +%s.%N)
    echo "$e - $s" | awk '{printf "%.3f", $1 - $3}'
}
t1=$(run "$n1")
t2=$(run "$n2")
awk -v a="$t1" -v b="$t2" -v n1="$n1" -v n2="$n2" -v g="$gpu" -v l="${launcher[*]}" -v la="$(cut -d' ' -f1 /proc/loadavg)" \
    'BEGIN { printf "gpu %s [%s]: %d frames %.3f s, %d frames %.3f s -> %.0f FPS (load %s)\n", g, l, n1, a, n2, b, (n2 - n1) / (b - a), la }'
