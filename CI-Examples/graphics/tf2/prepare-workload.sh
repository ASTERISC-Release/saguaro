#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
set -euo pipefail

if (($# != 2)) || [[ "$2" != "0" && "$2" != "23" ]]; then
    echo "usage: $0 TF2_DIR BOT_QUOTA" >&2
    exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TF2_DIR="$1"
bot_quota="$2"

install -m 0644 \
    "$SCRIPT_DIR/config/gramine_workload.cfg" \
    "$TF2_DIR/tf/cfg/gramine_workload.cfg"
install -m 0644 \
    "$SCRIPT_DIR/config/gramine_cp_badlands.cfg" \
    "$TF2_DIR/tf/cfg/cp_badlands.cfg"
install -d "$TF2_DIR/tf/scripts/vscripts"
install -m 0644 \
    "$SCRIPT_DIR/config/gramine_lock_points.nut" \
    "$TF2_DIR/tf/scripts/vscripts/gramine_lock_points.nut"

if [[ "$bot_quota" != "0" ]]; then
    sed -i -E "s/^tf_bot_quota [0-9]+$/tf_bot_quota $bot_quota/" \
        "$TF2_DIR/tf/cfg/gramine_workload.cfg" \
        "$TF2_DIR/tf/cfg/cp_badlands.cfg"
fi

for config in gramine_workload.cfg cp_badlands.cfg; do
    if ! grep -qx "tf_bot_quota $bot_quota" "$TF2_DIR/tf/cfg/$config"; then
        echo "error: failed to set bot quota in $config" >&2
        exit 1
    fi
done
