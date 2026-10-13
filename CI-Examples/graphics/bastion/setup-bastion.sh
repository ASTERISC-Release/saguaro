#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Unpack the game files of the GOG.com Linux installer of Bastion into ./bastion_game. The installer
# is NOT run: it is a Makeself/MojoSetup script with a zip archive appended, and only the zip's
# data/noarch/game/ directory is extracted (no root, nothing executed, nothing installed elsewhere).
#
#   ./setup-bastion.sh [path to bastion_*.sh]     default: ~/Downloads/bastion_*.sh
#
# The game is commercial and not part of this repository: buy and download "Bastion" for Linux from
# GOG.com (https://www.gog.com/game/bastion).
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
cd "$SCRIPT_DIR"

installer=${1:-}
if [ -z "$installer" ]; then
    installer=$(ls -1 "$HOME"/Downloads/bastion_*.sh 2>/dev/null | head -1 || true)
fi
if [ -z "$installer" ] || [ ! -f "$installer" ]; then
    echo "Bastion installer not found. Buy and download the Linux installer from GOG.com," >&2
    echo "then run: $0 /path/to/bastion_<version>.sh" >&2
    exit 1
fi

if [ -x bastion_game/Bastion.bin.x86_64 ]; then
    echo "bastion_game/ already holds the game; remove it to unpack again"
    exit 0
fi

echo "--- unpacking the game files of $installer (the installer is not executed)"
mkdir -p bastion_game
python3 -I - "$installer" bastion_game <<'PY'
import os, sys, zipfile
installer, dest = sys.argv[1], sys.argv[2]
prefix = "data/noarch/game/"
with zipfile.ZipFile(installer) as z:
    names = [n for n in z.namelist() if n.startswith(prefix) and not n.endswith("/")]
    if not names:
        sys.exit("no data/noarch/game/ in the archive: is this the GOG Linux installer?")
    for name in names:
        rel = os.path.normpath(name[len(prefix):])
        if rel.startswith(("..", "/")):
            sys.exit("refusing unsafe path in the archive: " + name)
        target = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with z.open(name) as src, open(target, "wb") as out:
            while chunk := src.read(1 << 20):
                out.write(chunk)
        mode = z.getinfo(name).external_attr >> 16
        if mode:
            os.chmod(target, mode & 0o777)
print(f"{len(names)} files")
PY
chmod +x bastion_game/Bastion.bin.x86_64
echo "done: ./bastion_game; next: make run-native, then make run"
