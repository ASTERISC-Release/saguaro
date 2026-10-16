#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Runs GE-Proton's Wine under gramine-direct or gramine-vm with test/proton-vm/wine.manifest.template
# (in the container, after run-native.sh -p made the prefix). Usage:
#   run-wine.sh [direct|vm] [ARGS...]      default args: --version
# Environment: WINEDEBUG (default -all), SYNC=1 (esync/fsync/ntsync on), LOG_LEVEL (default error),
# GRAMINE_CPU_NUM, GRAMINE_RAM_SIZE for the VM. The manifest and its scratch dirs are in WORK
# (default .proton-vm/wine-vm). Games (step 2 of "Next phases"): GAME=<dir> mounts the game's folder,
# DISPLAY_NUM=N gives the application the host X server :N through the vsock relay (the launcher's
# own DISPLAY must then be :N too), VK_ICD (default lavapipe), DXVK_DEVICE (default llvmpipe),
# DXVK_HUD, DXVK_CONFIG, FFMPEG_LIBS (default /opt/ffmpeg8/lib if it exists).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROTON=${PROTON:-$ROOT/.proton-vm/GE-Proton11-9-x86_64}
# the prefix: PREFIX, else the WINEPREFIX of wine-env.sh (the WoW64 one under WOW64=1), else the study's
PREFIX=${PREFIX:-${WINEPREFIX:-$ROOT/.proton-vm/work/compatdata/pfx}}
TESTS=${TESTS:-$ROOT/.proton-vm/tests}
WORK=${WORK:-$ROOT/.proton-vm/wine-vm}
LOADER=${1:-vm}; shift || true
[ $# -gt 0 ] || set -- --version
. "${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}/env.sh"
[ -n "${QEMU_PATH:-}" ] || { [ -x /root/qemu-tirith/build/qemu-system-x86_64 ] && export QEMU_PATH=/root/qemu-tirith/build/qemu-system-x86_64; }
mkdir -p "$WORK/tmp" "$WORK/home" "$WORK/shm" && cd "$WORK"
FFMPEG_LIBS=${FFMPEG_LIBS:-/opt/ffmpeg8/lib}; [ -d "$FFMPEG_LIBS" ] || FFMPEG_LIBS=
# GPU=1: the graphics redirection (design-gpu.md): AMD by default (EGL_DISCRETE=1, renderD129)
GPU=${GPU:-0}
if [ "$GPU" = 1 ]; then
    LAYER_DIR=$(python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/vulkan
    VK_ICD=${VK_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
    DXVK_DEVICE=${DXVK_DEVICE:-AMD}
    export GRAMINE_VM_GRAPHICS=1
    mkdir -p "$WORK/home/.cache"
fi
gramine-manifest -Dproton="$PROTON" -Dprefix="$PREFIX" -Dtests="$TESTS" -Dwork="$WORK" \
    -Dlog_level="${LOG_LEVEL:-error}" -Dwinedebug="${WINEDEBUG:--all}" -Dsync="${SYNC:-0}" \
    -Dgame="${GAME:-}" -Ddisplay="${DISPLAY_NUM:-}" -Dffmpeg="$FFMPEG_LIBS" \
    -Dvk_icd="${VK_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}" -Ddxvk_device="${DXVK_DEVICE:-llvmpipe}" \
    -Ddxvk_hud="${DXVK_HUD:-}" -Ddxvk_config="${DXVK_CONFIG:-}" -Ddxvk_log_level="${DXVK_LOG_LEVEL:-info}" -Dnet="${NET:-allow}" \
    -Dwinearch="${WINEARCH:-}" -Dgpu="$GPU" -Dgpu_preload="${GPU_PRELOAD:-0}" -Dgpu_eager="${GPU_EAGER:-0}" -Dlayer_dir="${LAYER_DIR:-}" -Degl_discrete="${EGL_DISCRETE:-1}" \
    "$HERE/wine.manifest.template" wine.manifest || exit 1
echo "# $(date -u +%Y-%m-%dT%H:%MZ) loader=$LOADER args: $* cpus=${GRAMINE_CPU_NUM:-1} ram=${GRAMINE_RAM_SIZE:-default} $(uptime | sed 's/.*load/load/')"
case $LOADER in
    direct) timeout "${TIMEOUT:-120}" gramine-direct wine "$@" ;;
    vm)     timeout "${TIMEOUT:-180}" gramine-vm wine "$@" 2>&1 | grep -v "self-check\|Frame-pool\|boot\]\|Physical\|Configuring\|REM_MEM\|vm-relay\|frame pool\|^(qemu)\|^qemu-system" ;;
esac
echo "# exit ${PIPESTATUS[0]}"
