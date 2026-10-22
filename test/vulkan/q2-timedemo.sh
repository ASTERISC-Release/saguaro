#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Quake II timedemo (633 frames of the demo's first map) with the Vulkan renderer ref_vk, natively,
# under saguaro-direct or under saguaro-vm, windowed 1920x1080 (dev docs: vulkan/, phase 3).
# Run inside the container, with Saguaro's env.sh sourced and, for vm, QEMU_PATH set.
#
# Usage: test/vulkan/q2-timedemo.sh native|direct|vm intel|amd [RUNS] [RENDERER]
#   RENDERER: vk (default), gl3 or gl1
# Prints one line per run with the game's own result ("633 frames, N seconds: N fps"), read from
# its console log (+set logfile 2 flushes every line; the game's stdout is lost when it is stopped).
set -u
mode=$1 gpu=$2 runs=${3:-3} ren=${4:-vk}
REPO=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
Q2=$REPO/CI-Examples/graphics/quake2
LOG=$HOME/.yq2/baseq2/qconsole.log
icd=/usr/share/vulkan/icd.d/$([ "$gpu" = amd ] && echo radeon || echo intel)_icd.json
discrete=$([ "$gpu" = amd ] && echo 1 || echo 0)
args=(+set vid_renderer "$ren" +set vid_fullscreen 0 +set r_mode -1 +set r_customwidth 1920
      +set r_customheight 1080 +set r_vsync 0 +set vk_vsync 0 +set logfile 2 +set vk_device 0 +vid_maxfps 999 +timedemo 1
      +map q2demo1.dm2)
mkdir -p "$HOME/.yq2/baseq2" "$HOME/.cache"
cd "$Q2"
case $mode in
    direct) make -s quake2-direct.manifest EGL_DISCRETE=$discrete >/dev/null ;;
    vm)
        make -s quake2.manifest VULKAN=$([ "$ren" = vk ] && echo 1 || echo 0) \
            EGL_DISCRETE=$discrete >/dev/null
        # the manifest's argv is fixed: put the benchmark's in
        python3 -I - "${args[@]}" <<'PY'
import json, re, sys
p = 'quake2.manifest'
s = open(p).read()
argv = json.dumps(['quake2'] + sys.argv[1:])
s2 = re.sub(r'argv = \[[^\]]*\]', 'argv = ' + argv, s, count=1)
assert s2 != s
open(p, 'w').write(s2)
PY
        ;;
esac
for r in $(seq 1 "$runs"); do
    rm -f "$LOG"
    case $mode in
        native) VK_ICD_FILENAMES=$icd DRI_PRIME=$discrete vblank_mode=0 SDL_AUDIODRIVER=dummy \
                    ALSOFT_DRIVERS=null unshare --user --map-user=1000 --map-group=1000 \
                    quake2_bin/release/quake2 "${args[@]}" >/dev/null 2>&1 & ;;
        direct) VK_ICD_FILENAMES=$icd vblank_mode=0 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
                    saguaro-direct quake2-direct "${args[@]}" >/dev/null 2>&1 & ;;
        vm) saguaro-vm quake2 > /tmp/q2-vm-run.log 2>&1 & ;;
    esac
    pid=$!
    start=$(date +%s)
    result=
    while [ $(( $(date +%s) - start )) -lt 120 ]; do
        result=$(grep -a " fps" "$LOG" 2>/dev/null | tail -1)
        [ -n "$result" ] && break
        sleep 1
    done
    kill "$pid" 2>/dev/null
    pkill -x quake2 2>/dev/null
    [ "$mode" = vm ] && pkill -x qemu-system-x86 2>/dev/null
    wait "$pid" 2>/dev/null
    echo "$mode $gpu $ren run $r: ${result:-no result in 120 s} (load $(cut -d' ' -f1 /proc/loadavg))"
done
[ "$mode" = vm ] && make -s -C "$Q2" quake2.manifest >/dev/null
exit 0
