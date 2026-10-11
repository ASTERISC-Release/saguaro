#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
set -euo pipefail

TF2_PID="${1:-}"
WINDOW_PATTERN="${TF2_WINDOW_PATTERN:-Team Fortress 2.*OpenGL}"
WINDOW_CLASS="${TF2_WINDOW_CLASS:-tf_linux64}"
STARTUP_DELAY="${TF2_AUTOINPUT_DELAY:-40}"
SEARCH_TIMEOUT="${TF2_AUTOINPUT_TIMEOUT:-120}"
STEP_DELAY="${TF2_AUTOINPUT_STEP_DELAY:-7}"
CLASS_DELAY="${TF2_AUTOINPUT_CLASS_DELAY:-7}"
POST_CLASS_DELAY="${TF2_AUTOINPUT_POST_CLASS_DELAY:-7}"
POSITION_DELAY="${TF2_AUTOINPUT_POSITION_DELAY:-7}"
POSITION_TRIGGER_DELAY="${TF2_AUTOINPUT_POSITION_TRIGGER_DELAY:-1}"
ATTEMPTS="${TF2_AUTOINPUT_ATTEMPTS:-3}"
RETRY_DELAY="${TF2_AUTOINPUT_RETRY_DELAY:-3}"
BENCHMARK_VIEW="${TF2_BENCHMARK_VIEW:-1}"
BENCHMARK_POS="${TF2_BENCHMARK_POS:--480 -4512 260.031311}"
BENCHMARK_ANGLES="${TF2_BENCHMARK_ANGLES:-0 90 0}"
DIRECT_INPUT="${TF2_AUTOINPUT_DIRECT:-0}"
READY_FILE="${TF2_AUTOINPUT_READY_FILE:-}"
READY_PATTERN="${TF2_AUTOINPUT_READY_PATTERN:-Client reached server_spawn.}"
PLAYER_PATTERN="${TF2_AUTOINPUT_PLAYER_PATTERN:-'soldier.cfg' not present; not executing.}"
READY_DELAY="${TF2_AUTOINPUT_READY_DELAY:-5}"
TEAM_MARKER="TF2_AUTOINPUT_TEAM_ACCEPTED"
POSITION_MARKER="TF2_AUTOINPUT_POSITION_ACCEPTED"
CLASS_CONFIRM_TIMEOUT="${TF2_AUTOINPUT_CLASS_CONFIRM_TIMEOUT:-10}"
COMMAND_CONFIRM_TIMEOUT="${TF2_AUTOINPUT_COMMAND_CONFIRM_TIMEOUT:-3}"

team_command="jointeam blue; echo $TEAM_MARKER"

if [[ "$BENCHMARK_VIEW" != 0 && "$BENCHMARK_VIEW" != 1 ]]; then
    echo "error: TF2_BENCHMARK_VIEW must be 0 or 1" >&2
    exit 2
fi
if [[ "$DIRECT_INPUT" != 0 && "$DIRECT_INPUT" != 1 ]]; then
    echo "error: TF2_AUTOINPUT_DIRECT must be 0 or 1" >&2
    exit 2
fi
if [[ ! "$READY_DELAY" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "error: TF2_AUTOINPUT_READY_DELAY must be a non-negative number" >&2
    exit 2
fi
if [[ ! "$POSITION_TRIGGER_DELAY" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "error: TF2_AUTOINPUT_POSITION_TRIGGER_DELAY must be a non-negative number" >&2
    exit 2
fi
if [[ "$BENCHMARK_VIEW" == 1 ]]; then
    if [[ -z "$BENCHMARK_POS" || -z "$BENCHMARK_ANGLES" ]]; then
        echo "error: TF2_BENCHMARK_POS and TF2_BENCHMARK_ANGLES must be set together" >&2
        exit 2
    fi

    read -r -a benchmark_pos_values <<<"$BENCHMARK_POS"
    read -r -a benchmark_angle_values <<<"$BENCHMARK_ANGLES"
    if ((${#benchmark_pos_values[@]} != 3 || ${#benchmark_angle_values[@]} != 3)); then
        echo "error: benchmark position and angles must each contain three numbers" >&2
        exit 2
    fi

    number_pattern='^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$'
    for value in "${benchmark_pos_values[@]}" "${benchmark_angle_values[@]}"; do
        if [[ ! "$value" =~ $number_pattern ]]; then
            echo "error: invalid TF2 benchmark coordinate: $value" >&2
            exit 2
        fi
    done

    position_command="sensitivity 0; setpos ${benchmark_pos_values[*]}; bind F11 \"setang ${benchmark_angle_values[*]}; getpos; echo $POSITION_MARKER\""
else
    position_command="echo $POSITION_MARKER"
fi

if ! command -v xdotool >/dev/null 2>&1; then
    echo "error: xdotool is required for unattended TF2 menu input" >&2
    exit 1
fi

deadline=$((SECONDS + SEARCH_TIMEOUT))
window_id=""
while (( SECONDS < deadline )); do
    if [[ -n "$TF2_PID" ]]; then
        window_id="$(xdotool search --onlyvisible --pid "$TF2_PID" 2>/dev/null | tail -n 1 || true)"
    else
        window_id="$(xdotool search --onlyvisible --name "$WINDOW_PATTERN" 2>/dev/null | tail -n 1 || true)"
        if [[ -z "$window_id" ]]; then
            window_id="$(xdotool search --onlyvisible --class "$WINDOW_CLASS" 2>/dev/null | tail -n 1 || true)"
        fi
    fi

    [[ -n "$window_id" ]] && break
    sleep 0.25
done

if [[ -z "$window_id" ]]; then
    echo "error: timed out waiting for the TF2 OpenGL window" >&2
    exit 1
fi

# The X window is created before map loading and initial shader compilation.
# When the launcher supplies its output log, wait until Source reports that the
# local TF server is loaded. Otherwise retain the fixed startup-delay fallback.
# Then follow the first Welcome panel's keyboard-only path:
# Welcome -> Map Info -> Soldier.
if [[ -n "$READY_FILE" ]]; then
    deadline=$((SECONDS + SEARCH_TIMEOUT))
    while (( SECONDS < deadline )); do
        if [[ -r "$READY_FILE" ]] && grep -Fq -- "$READY_PATTERN" "$READY_FILE"; then
            break
        fi
        sleep 0.25
    done
    if (( SECONDS >= deadline )); then
        echo "error: timed out waiting for TF2 readiness marker: $READY_PATTERN" >&2
        exit 1
    fi
    sleep "$READY_DELAY"
else
    sleep "$STARTUP_DELAY"
fi

key_arguments=(--clearmodifiers)
type_arguments=(--clearmodifiers --delay 1)
if [[ "$DIRECT_INPUT" == 1 ]]; then
    key_arguments=(--window "$window_id" --clearmodifiers)
    type_arguments=(--window "$window_id" --clearmodifiers --delay 1)
fi

send_console_command() {
    local command="$1"
    xdotool key "${key_arguments[@]}" grave 2>/dev/null
    sleep 0.25
    xdotool type "${type_arguments[@]}" "$command" 2>/dev/null
    xdotool key "${key_arguments[@]}" Return 2>/dev/null
    sleep 0.25
    xdotool key "${key_arguments[@]}" grave 2>/dev/null
}

wait_for_marker() {
    local marker="$1"
    local timeout="$2"
    local confirm_deadline

    [[ -n "$READY_FILE" ]] || return 0
    confirm_deadline=$((SECONDS + timeout))
    while (( SECONDS < confirm_deadline )); do
        if [[ -r "$READY_FILE" ]] && grep -Fq -- "$marker" "$READY_FILE"; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

xdotool windowfocus --sync "$window_id" 2>/dev/null
xdotool key "${key_arguments[@]}" Return 2>/dev/null
sleep "$STEP_DELAY"
xdotool key "${key_arguments[@]}" Return 2>/dev/null
sleep "$CLASS_DELAY"

send_console_command "$team_command"
if ! wait_for_marker "$TEAM_MARKER" "$COMMAND_CONFIRM_TIMEOUT"; then
    echo "error: TF2 never accepted the unattended BLUE-team command" >&2
    exit 1
fi
sleep "$RETRY_DELAY"

command_confirmed=0
for ((attempt = 1; attempt <= ATTEMPTS; attempt++)); do
    # A slow one-vCPU load can display the class screen before it accepts a
    # number key. Retrying only the Soldier key is harmless after spawning: in
    # gameplay it merely selects the secondary weapon.
    xdotool key "${key_arguments[@]}" 2 2>/dev/null
    if [[ -n "$READY_FILE" ]]; then
        if wait_for_marker "$PLAYER_PATTERN" "$CLASS_CONFIRM_TIMEOUT"; then
            command_confirmed=1
        fi
    else
        sleep "$POST_CLASS_DELAY"
        command_confirmed=1
    fi

    echo "sent unattended TF2 class input to window $window_id (attempt $attempt/$ATTEMPTS)" >&2
    (( command_confirmed == 1 )) && break
    if (( attempt < ATTEMPTS )); then
        sleep "$RETRY_DELAY"
    fi
done

if (( command_confirmed == 0 )); then
    echo "error: TF2 never selected the unattended Soldier class" >&2
    exit 1
fi

# A one-vCPU Gramine run can finish class selection while the Map Info overlay
# is still visible. Return closes that residual panel and is inert in gameplay.
xdotool key "${key_arguments[@]}" Return 2>/dev/null

# Source's `wait` command counts rendered frames, so it is not suitable for
# waiting on the player entity at benchmark frame rates. Use wall time before
# applying the fixed camera position.
sleep "$POSITION_DELAY"
position_confirmed=0
for ((attempt = 1; attempt <= ATTEMPTS; attempt++)); do
    send_console_command "$position_command"
    # Let the one-vCPU guest process the new binding before triggering it.
    # F11 applies the final view after Source has re-entered relative-mouse
    # mode, avoiding a one-time mouse-grab delta after setang.
    sleep "$POSITION_TRIGGER_DELAY"
    xdotool key "${key_arguments[@]}" F11 2>/dev/null
    if wait_for_marker "$POSITION_MARKER" "$COMMAND_CONFIRM_TIMEOUT"; then
        position_confirmed=1
        break
    fi
    if (( attempt < ATTEMPTS )); then
        sleep "$RETRY_DELAY"
    fi
done
if (( position_confirmed == 0 )); then
    echo "error: TF2 never accepted the unattended positioning command" >&2
    exit 1
fi
if [[ -n "$READY_FILE" && "$BENCHMARK_VIEW" == 1 ]]; then
    position_report="$(grep -E '^setpos [-+0-9.]+ [-+0-9.]+ [-+0-9.]+;setang [-+0-9.]+ [-+0-9.]+ [-+0-9.]+' "$READY_FILE" | tail -n 1 || true)"
    if [[ -z "$position_report" ]] || ! awk \
            -v expected_x="${benchmark_pos_values[0]}" \
            -v expected_y="${benchmark_pos_values[1]}" \
            -v expected_pitch="${benchmark_angle_values[0]}" \
            -v expected_yaw="${benchmark_angle_values[1]}" \
            -v expected_roll="${benchmark_angle_values[2]}" '
                function abs(value) { return value < 0 ? -value : value }
                {
                    split($4, field, ";")
                    valid = field[2] == "setang" &&
                            abs($2 - expected_x) < 0.001 &&
                            abs($3 - expected_y) < 0.001 &&
                            abs($5 - expected_pitch) < 0.001 &&
                            abs($6 - expected_yaw) < 0.001 &&
                            abs($7 - expected_roll) < 0.001
                    exit valid ? 0 : 1
                }
            ' <<<"$position_report"; then
        echo "error: TF2 did not report the requested benchmark position and view" >&2
        exit 1
    fi
fi
send_console_command 'con_logfile ""'
echo "positioned unattended TF2 benchmark player in window $window_id" >&2
