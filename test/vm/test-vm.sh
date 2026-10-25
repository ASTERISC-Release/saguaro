#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# test-vm.sh — the aarch64 `vm` PAL test battery (run it where /dev/kvm and stock
# qemu-system-aarch64 are available, e.g. the container from container/run.sh).
#
#   libos     the LibOS regression suite under saguaro-vm (SAGUARO_VM=1, libos/test/regression/vm_skip.toml)
#   loopback  host-initiated connections into a guest AF_INET TCP server (test/vm/network/test-vm-loopback.sh,
#             dev docs: CHANGELOG.md phase 1)
#   epoll     many small synchronous requests through an epoll-driven guest TCP server
#   outbound  guest-initiated TCP through the relay, policy denial, and direct graphics ports
#   udp       guest getaddrinfo and a host-side UDP echo through the relay
#   guest-loopback guest IPv4/IPv6 TCP and UDP loopback through the relay
#
# The CI-Examples run through their exec.sh (`CI-Examples/<name>/exec.sh --vm`, or test/quick.sh vm).
#
# Usage: test/vm/test-vm.sh [--prefix DIR] [--build-dir DIR] [suite...]   (default: libos loopback)
# Prerequisite: a vm build installed with `scripts/install.sh --vm` (default prefix ~/.local/saguaro-vm,
# build dir build-vm-<arch>; it builds the PIE test binaries the libos suite needs).
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
BUILD=build-vm-$(uname -m)
SUITES=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --build-dir) BUILD=$2; shift 2 ;;
        libos|loopback|epoll|outbound|udp|guest-loopback) SUITES+=("$1"); shift ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ ${#SUITES[@]} -gt 0 ] || SUITES=(libos loopback)

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
export VM_PREFIX=$PREFIX
rc=0
for s in "${SUITES[@]}"; do
    echo "=== $s"
    case $s in
    libos)
        [ -z "${PYTEST_K:-}${PYTEST_ARGS:-}" ] || echo "pytest selection: -k '${PYTEST_K:-}' ${PYTEST_ARGS:-}"
        (cd "$REPO/libos/test/regression" && SAGUARO_VM=1 SAGUARO_CPU_NUM=${SAGUARO_CPU_NUM:-2} \
            saguaro-test pytest -- -q -rfEsxX --tb=short -p no:cacheprovider test_libos.py ${PYTEST_K:+-k "$PYTEST_K"} ${PYTEST_ARGS:-}) || rc=1 ;;
    loopback)
        # Host-initiated connections into the guest (the driver is shared with aarch64; the test
        # itself has no architecture-specific part, but only x86_64 has been run with it so far).
        "$REPO/test/vm/network/test-vm-loopback.sh" --prefix "$PREFIX" || rc=1 ;;
    epoll)
        "$REPO/test/vm/network/test-vm-epoll.sh" --prefix "$PREFIX" || rc=1 ;;
    outbound)
        "$REPO/test/vm/network/test-vm-outbound.sh" --prefix "$PREFIX" || rc=1 ;;
    udp)
        "$REPO/test/vm/network/test-vm-udp.sh" --prefix "$PREFIX" || rc=1 ;;
    guest-loopback)
        "$REPO/test/vm/network/test-vm-guest-loopback.sh" --prefix "$PREFIX" || rc=1 ;;
    esac
done
[ $rc = 0 ] && echo "ALL VM SUITES PASSED" || echo "SOME VM SUITES FAILED"
exit $rc
