#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Fetch and build Yamagi Quake II into ./quake2_bin and install the game data of the Quake II demo
# (no root needed; the build dependencies are in the x86_64 container image).
#
#   YQ2_REV        git revision of yquake2 to build (default: QUAKE2_8_41, as in Tirith)
#   Q2_DEMO_URL    where to fetch id Software's Quake II demo (q2-314-demo-x86.exe, a self-extracting
#                  zip); only its baseq2/pak0.pak is used
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
YQ2_REV=${YQ2_REV:-QUAKE2_8_41}
Q2_DEMO_URL=${Q2_DEMO_URL:-https://deponie.yamagi.org/quake2/idstuff/q2-314-demo-x86.exe}
cd "$SCRIPT_DIR"

echo "--- yquake2 ($YQ2_REV)"
[ -d quake2_bin ] || git clone https://github.com/yquake2/yquake2.git quake2_bin
git -C quake2_bin checkout -q "$YQ2_REV"
make -C quake2_bin -j"$(nproc)"

echo "--- game data (Quake II demo)"
PAK=quake2_bin/release/baseq2/pak0.pak
if [ ! -f "$PAK" ]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    wget -q -O "$tmp/demo.exe" "$Q2_DEMO_URL"
    python3 -I - "$tmp/demo.exe" "$tmp/out" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    names = [n for n in z.namelist() if n.lower().endswith('baseq2/pak0.pak')]
    if len(names) != 1:
        sys.exit('pak0.pak not found in the demo archive')
    z.extract(names[0], sys.argv[2])
    print(names[0])
PY
    mkdir -p "$(dirname "$PAK")"
    find "$tmp/out" -iname pak0.pak -exec cp {} "$PAK" \;
fi
ls -l "$PAK"

echo "yquake2: $(git -C quake2_bin rev-parse HEAD)"
echo "built: $SCRIPT_DIR/quake2_bin/release/quake2"
