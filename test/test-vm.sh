#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# test-vm.sh — the aarch64 `vm` PAL test battery (run it where /dev/kvm and stock
# qemu-system-aarch64 are available, e.g. the container from docker/build-vm-aarch64.sh).
#
#   libos     the LibOS regression suite under gramine-vm (GRAMINE_VM=1, libos/test/regression/vm_skip.toml)
#   examples  CI-Examples applications (tools/run-vm-aarch64-examples.sh)
#   hello     the bring-up soak test (tools/vm-aarch64-stage/hello.c, SMP=2 and SMP=4)
#
# Usage: test/test-vm.sh [--prefix DIR] [--build-dir DIR] [suite...]   (default: all)
# Prerequisite: a vm build installed with `scripts/install.sh --vm` (its default prefix/build dir
# are used unless overridden); the libos suite needs the build to have -Dtests=enabled -Dtests_pie=true
# (see Documentation/devel/vm.md, "Regression suite").
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$HERE")
PREFIX=${GRAMINE_PREFIX:-/tmp/vm-prefix}
BUILD=build-vm-container
SUITES=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --build-dir) BUILD=$2; shift 2 ;;
        libos|examples|hello) SUITES+=("$1"); shift ;;
        -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ ${#SUITES[@]} -gt 0 ] || SUITES=(libos examples hello)

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
export VM_PREFIX=$PREFIX
rc=0
for s in "${SUITES[@]}"; do
    echo "=== $s"
    case $s in
    libos)
        (cd "$REPO/libos/test/regression" && GRAMINE_VM=1 GRAMINE_CPU_NUM=${GRAMINE_CPU_NUM:-2} \
            gramine-test pytest -- -q -rfEsxX --tb=short -p no:cacheprovider test_libos.py) || rc=1 ;;
    examples)
        "$REPO/tools/run-vm-aarch64-examples.sh" || rc=1 ;;
    hello)
        for n in 2 4; do
            ok=0
            for i in $(seq 10); do
                out=$(cd "$REPO" && BUILD=$BUILD SMP=$n RUN_TIMEOUT=60 tools/run-vm-aarch64-dev.sh 2>&1)
                echo "$out" | grep -q "sigreturn recovered: faults=1" && echo "$out" | grep -q "exited with code 0" \
                    && ok=$((ok+1))
            done
            echo "hello SMP=$n: $ok/10"
            [ "$ok" = 10 ] || rc=1
        done ;;
    esac
done
[ $rc = 0 ] && echo "ALL VM SUITES PASSED" || echo "SOME VM SUITES FAILED"
exit $rc
