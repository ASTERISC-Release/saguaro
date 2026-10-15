#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Runs GE-Proton's Wine under gramine-direct or gramine-vm with test/proton-vm/wine.manifest.template
# (in the container, after run-native.sh -p made the prefix). Usage:
#   run-wine.sh [direct|vm] [ARGS...]      default args: --version
# Environment: WINEDEBUG (default -all), SYNC=1 (esync/fsync/ntsync on), LOG_LEVEL (default error),
# GRAMINE_CPU_NUM, GRAMINE_RAM_SIZE for the VM. The manifest and its scratch dirs are in WORK
# (default .proton-vm/wine-vm).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROTON=${PROTON:-$ROOT/.proton-vm/GE-Proton11-9-x86_64}
PREFIX=${PREFIX:-$ROOT/.proton-vm/work/compatdata/pfx}
TESTS=${TESTS:-$ROOT/.proton-vm/tests}
WORK=${WORK:-$ROOT/.proton-vm/wine-vm}
LOADER=${1:-vm}; shift || true
[ $# -gt 0 ] || set -- --version
. "${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}/env.sh"
[ -n "${QEMU_PATH:-}" ] || { [ -x /root/qemu-tirith/build/qemu-system-x86_64 ] && export QEMU_PATH=/root/qemu-tirith/build/qemu-system-x86_64; }
mkdir -p "$WORK/tmp" "$WORK/home" "$WORK/shm" && cd "$WORK"
gramine-manifest -Dproton="$PROTON" -Dprefix="$PREFIX" -Dtests="$TESTS" -Dwork="$WORK" \
    -Dlog_level="${LOG_LEVEL:-error}" -Dwinedebug="${WINEDEBUG:--all}" -Dsync="${SYNC:-0}" \
    "$HERE/wine.manifest.template" wine.manifest || exit 1
echo "# $(date -u +%Y-%m-%dT%H:%MZ) loader=$LOADER args: $* cpus=${GRAMINE_CPU_NUM:-1} ram=${GRAMINE_RAM_SIZE:-default} $(uptime | sed 's/.*load/load/')"
case $LOADER in
    direct) timeout "${TIMEOUT:-120}" gramine-direct wine "$@" ;;
    vm)     timeout "${TIMEOUT:-180}" gramine-vm wine "$@" 2>&1 | grep -v "self-check\|Frame-pool\|boot\]\|Physical\|Configuring\|REM_MEM\|vm-relay\|frame pool\|^(qemu)\|^qemu-system" ;;
esac
echo "# exit ${PIPESTATUS[0]}"
