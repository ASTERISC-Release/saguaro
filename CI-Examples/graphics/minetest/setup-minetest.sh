#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Fetch and build Minetest 5.6.1 (with IrrlichtMt 1.9.0mt8) into ./luanti-5.6.1 and prepare a world
# for it (no root needed; the build dependencies are in the x86_64 container image).
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
SRC=luanti-5.6.1
cd "$SCRIPT_DIR"

if [ ! -d "$SRC" ]; then
    echo "--- sources"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    wget -q -O "$tmp/minetest.zip" https://github.com/minetest/minetest/archive/refs/tags/5.6.1.zip
    wget -q -O "$tmp/irrlicht.zip" https://github.com/minetest/irrlicht/archive/refs/tags/1.9.0mt8.zip
    unzip -q "$tmp/minetest.zip" -d "$tmp/minetest"
    unzip -q "$tmp/irrlicht.zip" -d "$tmp/irrlicht"
    # the top-level directory is minetest-5.6.1 or luanti-5.6.1, depending on the archive
    mv "$tmp"/minetest/*-5.6.1 "$SRC"
    mv "$tmp"/irrlicht/irrlicht-1.9.0mt8 "$SRC/lib/irrlichtmt"

    echo "--- patch"
    patch --batch -p1 -d "$SRC" < patches/minetest-source-changes.patch
fi

echo "--- build"
(cd "$SRC" && cmake . -DRUN_IN_PLACE=TRUE -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVER=FALSE \
    -DENABLE_GETTEXT=FALSE && make -j"$(nproc)")

echo "--- world"
mkdir -p "$SRC/worlds/world1"
cat > "$SRC/worlds/world1/world.mt" <<'WORLD'
gameid = devtest
world_name = world1
backend = dummy
player_backend = sqlite3
auth_backend = sqlite3
mod_storage_backend = sqlite3
WORLD

echo "built: $SCRIPT_DIR/$SRC/bin/minetest"
