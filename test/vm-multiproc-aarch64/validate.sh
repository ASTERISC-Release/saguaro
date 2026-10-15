#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# validate.sh -- the whole AArch64 multi-process check of a prefix, one guest at a time: boot
# self-checks (hello.sh, key on and off, 1/2/4 vCPUs), the probes (key on) at 1, 2 and 4 vCPUs,
# the process LTP groups (key on) at 1, 2 and 4 vCPUs, then key off: the VM LibOS suite and the
# examples runner at 2 vCPUs. Prints one summary line per item.
# Usage: test/vm-multiproc-aarch64/validate.sh --prefix DIR [--build-dir DIR] [--skip-ltp]
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PREFIX=; BUILD=build-vm-$(uname -m); SKIP_LTP=0
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --build-dir) BUILD=$2; shift 2 ;;
        --skip-ltp) SKIP_LTP=1; shift ;;
        *) echo "unknown argument $1"; exit 2 ;;
    esac
done
cd "$REPO"
echo "### hello (self-checks) $(date -u +%FT%TZ)"
"$HERE/hello.sh" --prefix "$PREFIX" 1 2 4
for c in 1 2 4; do
    echo "### probes, $c vCPU(s)"
    LINES_SHOWN=8 "$HERE/probes.sh" --prefix "$PREFIX" --cpus "$c" sh spawn signals forks-mem
done
if [ $SKIP_LTP = 0 ]; then
    for c in 1 2 4; do
        echo "### LTP process groups, key on, $c vCPU(s)"
        "$HERE/ltp-groups.sh" --prefix "$PREFIX" --tag mp --key on --cpus "$c"
    done
fi
echo "### VM LibOS suite, key off, 2 vCPUs ($(cut -d' ' -f1 /proc/loadavg))"
test/test-vm.sh --prefix "$PREFIX" --build-dir "$BUILD" libos 2>&1 | grep -E "passed|failed|FAILED" | tail -5
echo "### examples, key off, 2 vCPUs ($(cut -d' ' -f1 /proc/loadavg))"
VM_PREFIX=$PREFIX tools/run-vm-aarch64-examples.sh 2>&1 | grep -E "pass=|FAIL|HANG|PANIC"
echo "### done $(date -u +%FT%TZ)"
