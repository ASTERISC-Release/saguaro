#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Collect the aarch64 libraries that the python (NumPy/SciPy), nodejs and OpenBLAS applications
# would pack, into DIR/{node,numpy,scipy,ubuntu}, for x18-scan.py. Run on the aarch64 WSL
# machine. Nothing is installed: Ubuntu packages are downloaded with `apt-get download` (no sudo)
# and unpacked with dpkg-deb -x into DIR/ubuntu. Wheels come from PyPI (the aarch64 file of the
# same version as CI-Examples/python/Makefile); node from nodejs.org (as CI-Examples/nodejs).
# Usage: x18-fetch.sh DIR
set -eu
D=$(realpath -m "$1"); mkdir -p "$D"/{dl,node,numpy,scipy,ubuntu}; cd "$D/dl"
[ "$(uname -m)" = aarch64 ] || { echo "not aarch64" >&2; exit 1; }

fetch_wheel() {  # name version outdir
    url=$(python3 - "$1" "$2" <<'PY'
import json, sys, urllib.request
n, v = sys.argv[1:3]
d = json.load(urllib.request.urlopen(f"https://pypi.org/pypi/{n}/{v}/json"))
for f in d["urls"]:
    if "cp312-cp312" in f["filename"] and "aarch64" in f["filename"] and "musllinux" not in f["filename"]:
        print(f["url"], f["digests"]["sha256"]); break
PY
)
    set -- $url "$3"; [ -n "$1" ] || { echo "no aarch64 cp312 wheel for $3" >&2; return 1; }
    f=$(basename "$1"); [ -f "$f" ] || curl -sSLo "$f" "$1"
    echo "$2  $f" | sha256sum -c -
    unzip -qo "$f" -d "$3"
}
fetch_wheel numpy 2.5.4 "$D/numpy"
fetch_wheel scipy 1.18.1 "$D/scipy"

NODE=node-v24.22.0-linux-arm64.tar.xz
[ -f $NODE ] || curl -sSLO https://nodejs.org/dist/v24.22.0/$NODE
echo "fef00bb4d36e1c50104a9775a10359beee98f162b4104dd12472c75065c6a143  $NODE" | sha256sum -c -
tar -C "$D/node" -xJf $NODE --strip-components=2 node-v24.22.0-linux-arm64/bin/node

# Ubuntu 24.04 aarch64 packages: the C++/Fortran runtimes and OpenBLAS the apps pack or would pack
for p in libstdc++6 libgcc-s1 zlib1g libgfortran5 libquadmath0 libopenblas0-pthread libopenblas0-serial libgomp1 libffi8 libatomic1; do
    apt-get download "$p" >/dev/null 2>&1 || echo "apt-get download $p failed" >&2
done
for f in *.deb; do dpkg-deb -x "$f" "$D/ubuntu"; done
echo fetched
