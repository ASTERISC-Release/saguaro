#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Installs the host prerequisites for the mongodb example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to
# preview. Instead of adding MongoDB's third-party apt repository (needs a signing key and root),
# this downloads the official release tarball, verifies its SHA-256 and unpacks it under
# $MONGODB_PREFIX (default ~/.local/mongodb), then links mongod into ~/.local/bin.
#
# Env: MONGODB_VERSION (default 7.0.14), MONGODB_PREFIX, SKIP_MONGOD=1 (only install the client libs
# if you already have a mongod on PATH).
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_gramine

VER=${MONGODB_VERSION:-7.0.14}
PREFIX=${MONGODB_PREFIX:-$HOME/.local/mongodb}

# Runtime libraries the prebuilt mongod links against, plus tools for this script and the client test.
apt_install curl ca-certificates python3 python3-pip "$(first_available_apt libssl3t64 libssl3)" \
    "$(first_available_apt libcurl4t64 libcurl4)"
pip_user_install pymongo pymongo   # replaces mongosh in mongo_client_test.py

if [ "${SKIP_MONGOD:-0}" = 1 ] || command -v mongod >/dev/null; then
    prereq_log "mongod already on PATH: $(command -v mongod 2>/dev/null || echo skipped)"
    prereq_log "mongodb: prerequisites OK"
    exit 0
fi

case "$(uname -m)" in
    aarch64)
        # The official Arm64 builds require ARMv8.2-A (LSE atomics + dot product).
        grep -qw atomics /proc/cpuinfo && grep -qw asimddp /proc/cpuinfo \
            || prereq_die "MongoDB 7.0 on arm64 needs an ARMv8.2-A CPU (atomics + asimddp)"
        ARCH=aarch64 ;;
    x86_64) ARCH=x86_64 ;;
    *) prereq_die "unsupported architecture $(uname -m)" ;;
esac

NAME=mongodb-linux-$ARCH-ubuntu2204-$VER
URL=https://fastdl.mongodb.org/linux/$NAME.tgz
DEST=$PREFIX/$NAME
if [ ! -x "$DEST/bin/mongod" ]; then
    prereq_log "downloading $URL"
    if [ "${DRY_RUN:-0}" = 1 ]; then
        prereq_log "(dry run) download, verify sha256, unpack into $PREFIX"
    else
        TMP=$(mktemp -d)
        trap 'rm -rf "$TMP"' EXIT
        curl -fsSL -o "$TMP/m.tgz" "$URL"
        curl -fsSL -o "$TMP/m.sha256" "$URL.sha256"
        echo "$(cut -d' ' -f1 "$TMP/m.sha256")  $TMP/m.tgz" | sha256sum -c - \
            || prereq_die "checksum mismatch for $URL"
        mkdir -p "$PREFIX"
        tar -xzf "$TMP/m.tgz" -C "$PREFIX"
    fi
fi
if [ "${DRY_RUN:-0}" != 1 ]; then
    mkdir -p "$HOME/.local/bin"
    ln -sf "$DEST/bin/mongod" "$HOME/.local/bin/mongod"
    "$DEST/bin/mongod" --version | head -1
    case ":$PATH:" in
        *":$HOME/.local/bin:"*) ;;
        *) prereq_log "NOTE: add \$HOME/.local/bin to PATH (or: export PATH=$DEST/bin:\$PATH)" ;;
    esac
fi
prereq_log "mongodb: prerequisites OK"
