#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Fetch and build SuperTuxKart from source into ./stk-build (no root needed; the build dependencies
# are in the x86_64 container image, see container/Containerfile.vm-x86_64).
#
#   STK_CODE_REV    git revision of stk-code to build (default: the remote's default branch)
#   STK_ASSETS_REV  svn revision of stk-assets (default: HEAD)
#
# The assets checkout is large (about 700 MB or more) and takes a while.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
PATCH_DIR=$SCRIPT_DIR/patches
mkdir -p ~/.config
mkdir -p "$SCRIPT_DIR/stk-build"
cd "$SCRIPT_DIR/stk-build"

echo "--- stk-code"
[ -d stk-code ] || git clone https://github.com/supertuxkart/stk-code.git
if [ -n "${STK_CODE_REV:-}" ]; then
    git -C stk-code checkout -q "$STK_CODE_REV"
fi

echo "--- stk-assets"
if [ ! -d stk-assets ]; then
    svn checkout -q ${STK_ASSETS_REV:+-r "$STK_ASSETS_REV"} \
        https://svn.code.sf.net/p/supertuxkart/code/stk-assets stk-assets
fi

echo "--- patches"
cd stk-code
for patch_file in "$PATCH_DIR"/*.patch; do
    if git apply --check "$patch_file" 2>/dev/null; then
        git apply "$patch_file"
    elif git apply --reverse --check "$patch_file" 2>/dev/null; then
        echo "already applied: $(basename "$patch_file")"
    else
        echo "failed to apply: $patch_file" >&2
        exit 1
    fi
done

echo "--- build"
mkdir -p cmake_build
cd cmake_build
cmake .. -DCMAKE_BUILD_TYPE=Release -DNO_SHADERC=on
make -j"$(nproc)"

echo "stk-code:   $(git -C "$SCRIPT_DIR/stk-build/stk-code" rev-parse HEAD)"
echo "stk-assets: r$(svn info --show-item revision "$SCRIPT_DIR/stk-build/stk-assets")"
echo "built: $PWD/bin/supertuxkart"
