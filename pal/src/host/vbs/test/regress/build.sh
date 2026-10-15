#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Build the runner and the LibOS regression programs that report "TEST OK" with scripts/vbs/vbs-gcc
# (dynamically linked against the vbs glibc). Programs that do not build this way are listed and
# skipped. Usage (aarch64 build container): build.sh OUTDIR
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../../.." && pwd)
OUT=$(realpath -m "$1")
REG="$ROOT/libos/test/regression"
CC="$ROOT/scripts/vbs/vbs-gcc"
mkdir -p "$OUT/tests"
$CC -O2 -o "$OUT/runner" "$HERE/runner.c"
ok=0; bad=""
for c in $(grep -l 'TEST OK' "$REG"/*.c); do
    t=$(basename "$c" .c)
    case "$t" in rw_file|dump) continue ;; esac
    if $CC -O2 -w -I"$REG" -I"$ROOT/common/include" -o "$OUT/tests/$t" "$c" "$REG/rw_file.c" \
            -pthread -lm 2>/dev/null; then
        ok=$((ok + 1))
    else
        bad="$bad $t"
    fi
done
# the vbs reproducers of this directory (platform-issues.md)
for c in "$HERE"/*.c; do
    t=$(basename "$c" .c)
    [ "$t" = runner ] && continue
    $CC -O2 -o "$OUT/tests/$t" "$c"
done
# helpers that the suite mounts at fixed paths (packed at /exec_victim)
$CC -O2 -w -o "$OUT/exec_victim" "$REG/exec_victim.c"
echo "built $ok programs; not built:$bad"
