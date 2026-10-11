#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run-examples.sh -- run example suites of test/test-direct.sh for riscv64 in the test guest.
#
# Usage (inside the cross container, guest started, build installed):
#   scripts/riscv64/run-examples.sh [suite...]      e.g.  helloworld multithread sqlite
#
# The examples are built natively in the guest, in a copy of the tracked files of CI-Examples/,
# test/ and scripts/ on the guest's own disk (/srv/gramine-examples). Building them in the
# repository itself would overwrite the x86_64 example binaries and manifests there, and the 9p
# share is a poor place for the files the examples create. The copy is refreshed on every run;
# what the guest built in it is kept.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILD="${BUILD:-$REPO/build-riscv64}"
DEST=/srv/gramine-examples

ARGS=""
for a in "$@"; do ARGS+=" $(printf '%q' "$a")"; done

(cd "$REPO" && git ls-files -z CI-Examples test scripts | tar -cf - --null -T -) \
    | "$HERE/guest.sh" ssh "mkdir -p $DEST && tar -xf - -C $DEST"

exec "$HERE/guest.sh" ssh "
    cd $DEST
    export GRAMINE_PREFIX=$BUILD/prefix
    export LOGDIR=/srv/gramine-examples-logs
    test/test-direct.sh $ARGS
"
