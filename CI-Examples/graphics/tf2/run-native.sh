#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TF2_DIR="${TF2_DIR:-$SCRIPT_DIR/game}"
TF2_NATIVE_HOME="${TF2_NATIVE_HOME:-$HOME}"
TF2_CPU="${TF2_CPU:-1}"
# The graphics preload library is installed with Gramine (-Dgraphics=enabled and a VM-type PAL).
GRAPHICS_LIB="${GRAPHICS_LIB:-$(python3 -c 'import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)')/libgraphics.so}"
tf2_pid=""
auto_input_pid=""
readiness_log=""

usage() {
    echo "usage: $0 <graphics-library 0|1> <limit-cpu 0|1> [--res=WIDTHxHEIGHT] [--bots] [TF2 arguments...]" >&2
    exit 2
}

if (($# < 2)) || [[ "$1" != "0" && "$1" != "1" ]] ||
        [[ "$2" != "0" && "$2" != "1" ]]; then
    usage
fi
use_graphics_lib="$1"
limit_cpu="$2"
shift 2
width=1920
height=1080
bots=0
tf2_arguments=()
for argument in "$@"; do
    case "$argument" in
        --res=*)
            resolution="${argument#--res=}"
            if [[ ! "$resolution" =~ ^[1-9][0-9]*x[1-9][0-9]*$ ]]; then
                echo "error: resolution must be WIDTHxHEIGHT" >&2
                usage
            fi
            width="${resolution%x*}"
            height="${resolution#*x}"
            ;;
        --bots)
            bots=1
            ;;
        *)
            tf2_arguments+=("$argument")
            ;;
    esac
done

cleanup() {
    if [[ -n "$auto_input_pid" ]]; then
        kill "$auto_input_pid" 2>/dev/null || true
        wait "$auto_input_pid" 2>/dev/null || true
    fi

    if [[ -n "$tf2_pid" ]] && kill -0 "$tf2_pid" 2>/dev/null; then
        kill -TERM "$tf2_pid" 2>/dev/null || true
        for _ in {1..5}; do
            if ! kill -0 "$tf2_pid" 2>/dev/null; then
                break
            fi
            sleep 0.2
        done
        kill -KILL "$tf2_pid" 2>/dev/null || true
        wait "$tf2_pid" 2>/dev/null || true
    fi
    if [[ -n "$readiness_log" ]]; then
        rm -f -- "$readiness_log"
    fi
}
trap cleanup EXIT INT TERM

if [[ ! -x "$TF2_DIR/tf_linux64" ]]; then
    echo "error: missing $TF2_DIR/tf_linux64; run ./setup-tf2.sh STEAM_USERNAME" >&2
    exit 1
fi

if [[ "${TF2_AUTOINPUT:-1}" != "0" ]] && ! command -v xdotool >/dev/null 2>&1; then
    echo "error: xdotool is required unless TF2_AUTOINPUT=0 is set" >&2
    exit 1
fi

if [[ "$limit_cpu" == "1" ]] &&
        { [[ ! "$TF2_CPU" =~ ^[0-9]+$ ]] ||
          ! command -v taskset >/dev/null 2>&1 ||
          ! taskset -c "$TF2_CPU" true 2>/dev/null; }; then
    echo "error: TF2_CPU must identify one CPU available to this environment" >&2
    exit 1
fi

export LD_LIBRARY_PATH="$TF2_DIR/bin/linux64:$TF2_DIR/tf/bin/linux64:$TF2_DIR:/lib:/usr/lib:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu"
export SteamAppId=440
export SteamGameId=440
# The Linux Steam overlay is normally injected through LD_PRELOAD.  Keep the
# benchmark deterministic even if this launcher was itself started from a
# Steam-managed environment, and tell Steam not to draw overlay UI if one of
# its libraries attempts to initialize the overlay through SteamAPI.
export SteamNoOverlayUIDrawing=1
export HOME="$TF2_NATIVE_HOME"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_DATA_HOME="$HOME/.local/share"
export XDG_CACHE_HOME="$HOME/.cache"
export GLIBC_TUNABLES="glibc.pthread.rseq=0"
export GALLIUM_THREAD=0
export __GL_SYNC_TO_VBLANK=0
export vblank_mode=0
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=x11
export PULSE_SERVER=none
export PIPEWIRE_REMOTE=none
export GBM_BACKEND=dri
export EGL_DISCRETE=0
export EGL_SYSCALL_LOG=0
export EGL_SANITY_FULL=0
export EGL_IOCTL_LOG=0
export EGL_DUMP_PNG=0
export MESA_LOG=0
export FONTCONFIG_FILE=/etc/fonts/fonts.conf
export FONTCONFIG_PATH=/etc/fonts

if [[ ! -e "$HOME/.steam/steam.pipe" ]]; then
    cat >&2 <<EOF
warning: $HOME/.steam/steam.pipe is unavailable.
The current TF2 Linux client needs a running desktop Steam client even for this
offline local workload. Run this launcher on the host as the Steam user, or set
TF2_NATIVE_HOME to that user's mounted home directory. A missing Steam IPC pipe
normally results in a permanently black window.
EOF
fi

make -s -C "$SCRIPT_DIR" fps_metrics.so
preload_libraries="$SCRIPT_DIR/fps_metrics.so"
if [[ "$use_graphics_lib" == "1" ]]; then
    preload_libraries="$preload_libraries:$GRAPHICS_LIB"
fi
export FPS_STATS_INTERVAL_SEC="${FPS_STATS_INTERVAL_SEC:-60}"
export FPS_STATS_WARMUP_SEC="${FPS_STATS_WARMUP_SEC:-45}"

bot_quota=0
if [[ "$bots" == "1" ]]; then
    bot_quota=23
fi
"$SCRIPT_DIR/prepare-workload.sh" "$TF2_DIR" "$bot_quota"
cd "$TF2_DIR"
tf2_command=(env "LD_PRELOAD=$preload_libraries" ./tf_linux64)
if [[ "$limit_cpu" == "1" ]]; then
    tf2_command=(taskset -c "$TF2_CPU" env "LD_PRELOAD=$preload_libraries" ./tf_linux64)
fi
if [[ "${TF2_AUTOINPUT:-1}" != "0" ]]; then
    readiness_log="$TF2_DIR/tf/gramine_ready.log"
    rm -f -- "$readiness_log"
fi
"${tf2_command[@]}" \
    -game tf \
    -steam \
    -insecure \
    -novid \
    -nobreakpad \
    -nominidumps \
    -nojoy \
    -nosteamcontroller \
    -nohltv \
    -noip \
    -nosound \
    -gl \
    -windowed \
    -w "$width" \
    -h "$height" \
    +exec gramine_workload \
    "${tf2_arguments[@]}" &
tf2_pid=$!

if [[ "${TF2_AUTOINPUT:-1}" != "0" ]]; then
    TF2_AUTOINPUT_DIRECT=1 TF2_AUTOINPUT_READY_FILE="$readiness_log" \
        "$SCRIPT_DIR/auto-input.sh" "$tf2_pid" &
    auto_input_pid=$!
fi

set +e
wait "$tf2_pid"
status=$?
set -e
exit "$status"
