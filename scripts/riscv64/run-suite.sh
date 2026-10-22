#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run-suite.sh -- run a Gramine regression suite for riscv64 in the test guest.
#
# Usage (inside the cross container, guest started, build installed and manifests rendered with
# render-test-manifests.sh):
#   scripts/riscv64/run-suite.sh pal   [pytest arguments...]
#   scripts/riscv64/run-suite.sh libos [pytest arguments...]     e.g.  -k 'eventfd or signal'
#
# SAGUARO_TEST_TIMEOUT (seconds per test, default 120) is taken from the caller's environment.
#
# The suite runs from a directory on the guest's own disk that mirrors
# build-riscv64/run/<suite>/regression with symbolic links. The files the tests create (tmp/,
# tmp_enc/, ...) must not live on the 9p share of the repository: it cannot fchmod() an unlinked
# file or keep an encrypted file across rename, and several file tests fail there for that reason
# alone.
set -euo pipefail

SUITE="${1:?usage: run-suite.sh <pal|libos> [pytest arguments...]}"
shift
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILD="${BUILD:-$REPO/build-riscv64}"

case "$SUITE" in
    pal)   TEST="$REPO/pal/regression/test_pal.py" ;;
    libos) TEST="$REPO/libos/test/regression/test_libos.py" ;;
    *) echo "unknown suite: $SUITE" >&2; exit 2 ;;
esac
RUN="$BUILD/run/$SUITE/regression"
[ -d "$RUN" ] || { echo "missing $RUN: run scripts/riscv64/render-test-manifests.sh $SUITE first" >&2; exit 1; }
LOCAL="/var/tmp/gramine-run/$SUITE/regression"

ARGS=""
for a in "$@"; do ARGS+=" $(printf '%q' "$a")"; done

exec "$HERE/guest.sh" ssh "
    set -e
    rm -rf $LOCAL && mkdir -p \$(dirname $LOCAL) && cp -as $RUN $LOCAL
    rm -rf $LOCAL/tmp/* $LOCAL/tmp_enc/*
    cd $LOCAL
    export PATH=$BUILD/prefix/bin:\$PATH
    export PYTHONPATH=\$(echo $BUILD/prefix/lib/python3*/site-packages)
    export SAGUARO_TEST_TIMEOUT=${SAGUARO_TEST_TIMEOUT:-120}
    python3 -m pytest $TEST -q --tb=line -rf -p no:cacheprovider $ARGS
"
