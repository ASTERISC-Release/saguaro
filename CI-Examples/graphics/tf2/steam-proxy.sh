#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
set -euo pipefail

STEAM_PORT="${STEAM_PROXY_PORT:-57343}"

if ! command -v socat >/dev/null 2>&1; then
    echo "error: socat is required for the Steam VSOCK relay" >&2
    exit 1
fi

if command -v ss >/dev/null 2>&1 &&
        ! ss -H -ltn "sport = :$STEAM_PORT" 2>/dev/null | grep -q .; then
    cat >&2 <<EOF
error: desktop Steam is not listening on TCP 127.0.0.1:$STEAM_PORT.
Start Steam and sign in before launching the TF2 relay.
EOF
    exit 1
fi

echo "Steam VSOCK relay active: guest port $STEAM_PORT -> 127.0.0.1:$STEAM_PORT"
exec socat -b 1048576 -d -d \
    "VSOCK-LISTEN:$STEAM_PORT,reuseaddr,fork,max-children=20" \
    "TCP4:127.0.0.1:$STEAM_PORT,nodelay"
