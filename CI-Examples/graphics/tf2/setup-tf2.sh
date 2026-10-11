#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Download the official native Linux TF2 client with SteamCMD.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
STEAMCMD_DIR="${STEAMCMD_DIR:-$SCRIPT_DIR/.steamcmd}"
TF2_DIR="${TF2_DIR:-$SCRIPT_DIR/game}"
STEAMCMD_URL="https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz"

usage() {
    cat >&2 <<'EOF'
usage: ./setup-tf2.sh [STEAM_USERNAME | --anonymous]

With a username, SteamCMD prompts for the password and Steam Guard code; secrets
are not accepted as command-line arguments. --anonymous is retained as a depot
permission diagnostic, but Valve currently serves only TF2's 806 KiB bootstrap
depot to anonymous SteamCMD sessions, not the Linux client or game assets.
EOF
    exit 2
}

case "${1:-}" in
    -h|--help)
        usage
        ;;
    --anonymous|"")
        STEAM_LOGIN=(anonymous)
        ;;
    --*)
        usage
        ;;
    *)
        STEAM_LOGIN=("$1")
        ;;
esac

mkdir -p "$STEAMCMD_DIR" "$TF2_DIR"

if [[ ! -x "$STEAMCMD_DIR/steamcmd.sh" ]]; then
    archive="$(mktemp "${TMPDIR:-/tmp}/steamcmd.XXXXXX.tar.gz")"
    trap 'rm -f "$archive"' EXIT
    echo "Downloading SteamCMD from Valve..."
    curl --fail --location --retry 3 "$STEAMCMD_URL" --output "$archive"
    tar -xzf "$archive" -C "$STEAMCMD_DIR"
fi

echo "Installing native Linux TF2 app 440 into $TF2_DIR"
"$STEAMCMD_DIR/steamcmd.sh" \
    +@ShutdownOnFailedCommand 1 \
    +@sSteamCmdForcePlatformType linux \
    +force_install_dir "$TF2_DIR" \
    +login "${STEAM_LOGIN[@]}" \
    +app_license_request 440 \
    +app_update 440 validate \
    +quit

if [[ ! -x "$TF2_DIR/tf_linux64" || ! -f "$TF2_DIR/tf/bin/linux64/client.so" ]]; then
    cat >&2 <<'EOF'

SteamCMD completed, but the native Linux client depot is absent.

Anonymous sessions currently receive only depot 440 (the 806 KiB bootstrap).
Run this script again with a Steam username that has claimed the free TF2
license. SteamCMD will prompt interactively for credentials and Steam Guard:

    ./setup-tf2.sh STEAM_USERNAME

No password is accepted by this script or placed in tracked repository files.
SteamCMD may cache local authentication state under the ignored .steamcmd/ dir.
EOF
    exit 1
fi

printf '440\n' > "$TF2_DIR/steam_appid.txt"
mkdir -p "$SCRIPT_DIR/home/.config" "$SCRIPT_DIR/home/.local/share" "$SCRIPT_DIR/home/.cache"

echo "TF2 Linux client is ready. Next: make, then see README.md"
