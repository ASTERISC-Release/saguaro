#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# direct-prefix-sweep.sh — run the fork_and_exec LibOS regression test under saguaro-direct from
# copies of one install at paths of every length from that of SWEEP_DIR + 2 up to MAX characters.
#
# What the PAL allocates at startup (paths, the manifest, the environment) decides where its heap
# runs out of free slots. A LibOS that allocates PAL handles at the wrong moment of its start then
# fails only for some lengths of the install path (dev docs: vm-multiproc/FINDINGS.md,
# 2026-10-15: 036a982b faulted with the default prefix ~/.local/saguaro, 20 characters, and not with
# the CI prefix). This sweep makes such a fault show up whatever prefix the developer or CI uses.
#
# The copies are hard links (cp -al; plain copies when SWEEP_DIR is on another file system) under
# SWEEP_DIR; each is removed after its run. Needs the rendered manifest
# libos/test/regression/fork_and_exec.manifest (run `test/direct/test-direct.sh libos` once first).
#
# Usage: test/direct/direct-prefix-sweep.sh
# Environment: SAGUARO_PREFIX (default ~/.local/saguaro), SWEEP_DIR (default /tmp/gps), MAX (longest
# path, default 72).
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
PREFIX=$(readlink -f "${SAGUARO_PREFIX:-$HOME/.local/saguaro}")
SWEEP_DIR=${SWEEP_DIR:-/tmp/gps}
MAX=${MAX:-72}
REG=$REPO/libos/test/regression
MAN=$REG/fork_and_exec.manifest

[ -f "$MAN" ] || { echo "no $MAN: run test/direct/test-direct.sh libos first" >&2; exit 2; }
grep -q "file:$PREFIX/" "$MAN" \
    || { echo "$MAN is rendered for another install: run test/direct/test-direct.sh libos" >&2; exit 2; }
LIBDIR=$(cd "$PREFIX" && ls -d lib/*/gramine/direct | head -n 1)
LIBDIR=${LIBDIR%/direct}
[ -n "$LIBDIR" ] || { echo "no direct PAL under $PREFIX" >&2; exit 2; }

mkdir -p "$SWEEP_DIR"
fail=0 runs=0
for len in $(seq $(( ${#SWEEP_DIR} + 2 )) "$MAX"); do
    pfx=$SWEEP_DIR/$(head -c $(( len - ${#SWEEP_DIR} - 1 )) /dev/zero | tr '\0' p)
    rm -rf "$pfx"
    mkdir -p "$pfx/$(dirname "$LIBDIR")"
    cp -al "$PREFIX/$LIBDIR" "$pfx/$LIBDIR" 2>/dev/null || cp -a "$PREFIX/$LIBDIR" "$pfx/$LIBDIR"
    sed "s|file:$PREFIX/|file:$pfx/|g" "$MAN" > "$REG/prefix_sweep.manifest"
    out=$(cd "$REG" && timeout -s KILL 60 "$pfx/$LIBDIR/direct/loader" "$pfx/$LIBDIR/direct/libpal.so" \
          init prefix_sweep 2>&1) || true
    runs=$((runs + 1))
    if ! grep -q "^child exited with status: 0" <<<"$out"; then
        echo "FAIL path length $len ($pfx):"
        tail -n 3 <<<"$out" | sed 's/^/  /'
        fail=$((fail + 1))
    fi
    rm -rf "$pfx"
done
rm -f "$REG/prefix_sweep.manifest"
echo "prefix sweep: $((runs - fail))/$runs path lengths passed (fork_and_exec, $PREFIX)"
[ "$fail" = 0 ]
