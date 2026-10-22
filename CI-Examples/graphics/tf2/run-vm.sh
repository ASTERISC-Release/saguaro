#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TF2_DIR="${TF2_DIR:-$SCRIPT_DIR/game}"
auto_input_pid=""
readiness_log=""

usage() {
    echo "usage: $0 [--res=WIDTHxHEIGHT] [--bots]" >&2
    exit 2
}

width=1920
height=1080
bots=0
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
        -h|--help)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

if [[ "${TF2_SKIP_STEAM_PREFLIGHT:-0}" != "1" ]] && command -v ss >/dev/null 2>&1; then
    if ! ss -H -ltn "sport = :57343" 2>/dev/null | grep -q .; then
        cat >&2 <<'EOF'
error: desktop Steam is not listening on TCP port 57343.
Start Steam and sign in before running TF2.
EOF
        exit 1
    fi

    if ! ss -H -ln --vsock "sport = :57343" 2>/dev/null | grep -q .; then
        cat >&2 <<'EOF'
error: the Steam VSOCK relay is not listening on port 57343.
Run this in a second terminal (see README.md):

    ./steam-proxy.sh

The X11 proxy on port 6000 does not forward Steam IPC.
EOF
        exit 1
    fi
fi

make -s -C "$SCRIPT_DIR" TF2_WIDTH="$width" TF2_HEIGHT="$height" tf2.manifest

bot_quota=0
if [[ "$bots" == "1" ]]; then
    bot_quota=23
fi
"$SCRIPT_DIR/prepare-workload.sh" "$TF2_DIR" "$bot_quota"

if [[ "${TF2_AUTOINPUT:-1}" != "0" ]] && ! command -v xdotool >/dev/null 2>&1; then
    echo "error: xdotool is required unless TF2_AUTOINPUT=0 is set" >&2
    exit 1
fi

cleanup() {
    if [[ -n "$auto_input_pid" ]]; then
        kill "$auto_input_pid" 2>/dev/null || true
    fi
    if [[ -n "$readiness_log" ]]; then
        rm -f -- "$readiness_log"
    fi
}
trap cleanup EXIT INT TERM

if [[ "${TF2_AUTOINPUT:-1}" != "0" ]]; then
    readiness_log="$TF2_DIR/tf/gramine_ready.log"
    rm -f -- "$readiness_log"
    TF2_AUTOINPUT_DIRECT=1 TF2_AUTOINPUT_READY_FILE="$readiness_log" \
        "$SCRIPT_DIR/auto-input.sh" &
    auto_input_pid=$!
fi

set +e
export SAGUARO_RAM_SIZE="${SAGUARO_RAM_SIZE:-16G}"
export SAGUARO_CPU_NUM="${SAGUARO_CPU_NUM:-1}"
gramine-vm tf2
status=$?
set -e
exit "$status"
