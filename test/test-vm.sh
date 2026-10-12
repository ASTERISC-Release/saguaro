#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# test-vm.sh — the aarch64 `vm` PAL test battery (run it where /dev/kvm and stock
# qemu-system-aarch64 are available, e.g. the container from container/run.sh).
#
#   libos     the LibOS regression suite under gramine-vm (GRAMINE_VM=1, libos/test/regression/vm_skip.toml)
#   examples  CI-Examples applications (tools/run-vm-aarch64-examples.sh)
#   hello     the bring-up soak test (tools/vm-aarch64-stage/hello.c, SMP=2 and SMP=4)
#   loopback  host-initiated connections into a guest AF_INET TCP server (test/test-vm-loopback.sh,
#             Documentation/devel/vm-network/ phase 1)
#   epoll     many small synchronous requests through an epoll-driven guest TCP server
#   outbound  guest-initiated TCP through the relay, policy denial, and direct graphics ports
#
# Usage: test/test-vm.sh [--prefix DIR] [--build-dir DIR] [suite...]   (default: all)
# Prerequisite: a vm build installed with `scripts/install.sh --vm` (default prefix ~/.local/gramine-vm,
# build dir build-vm-<arch>; it builds the PIE test binaries the libos suite needs).
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$HERE")
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
BUILD=build-vm-$(uname -m)
SUITES=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --build-dir) BUILD=$2; shift 2 ;;
        libos|examples|hello|loopback|epoll|outbound) SUITES+=("$1"); shift ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ ${#SUITES[@]} -gt 0 ] || SUITES=(libos examples hello loopback)

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
        # The bring-up soak test targets KVM on a native aarch64 host; on anything else
        # it cannot boot (run-vm-aarch64-dev.sh does not emulate) and would count as failures.
        if [ "$(uname -m)" != "aarch64" ]; then
            echo "hello: skipped (aarch64-only bring-up test; this host is $(uname -m))"
            continue
        fi
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
    loopback)
        # Host-initiated connections into the guest (the driver is shared with aarch64; the test
        # itself has no architecture-specific part, but only x86_64 has been run with it so far).
        "$REPO/test/test-vm-loopback.sh" --prefix "$PREFIX" || rc=1 ;;
    epoll)
        "$REPO/test/test-vm-epoll.sh" --prefix "$PREFIX" || rc=1 ;;
    outbound)
        "$REPO/test/test-vm-outbound.sh" --prefix "$PREFIX" || rc=1 ;;
    esac
done
[ $rc = 0 ] && echo "ALL VM SUITES PASSED" || echo "SOME VM SUITES FAILED"
exit $rc
