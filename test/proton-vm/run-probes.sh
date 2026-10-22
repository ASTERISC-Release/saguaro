#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Runs test/proton-vm/probes.c natively, under gramine-direct and under gramine-vm (in the
# container, after scripts/install.sh --vm). Usage: run-probes.sh [native|direct|vm] [PROBE...]
# (default: all probes). PROBES_SRC selects another source (test/vm/timer/timerprobes.c on aarch64). Output: the PROBE lines; WORK (default .proton-vm/probes) holds the
# binary, the manifests and /tmp of the probes. SAGUARO_CPU_NUM and SAGUARO_RAM_SIZE apply to vm.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=${WORK:-$ROOT/.proton-vm/probes}
LOADER=${1:-vm}; shift || true
. "${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}/env.sh"
[ -n "${QEMU_PATH:-}" ] || { [ -x /root/qemu-tirith/build/qemu-system-x86_64 ] && export QEMU_PATH=/root/qemu-tirith/build/qemu-system-x86_64; }
mkdir -p "$WORK/tmp" && cd "$WORK"
gcc -O1 -g -pthread -fno-stack-protector -o probes "${PROBES_SRC:-$HERE/probes.c}" 2>&1 | grep -v "Wunused-result\|^ *|\|^ *[0-9]* |\|^ *\^\|In function"
gramine-manifest -Darch_libdir=/lib/$(uname -m)-linux-gnu -Dentry=/probe/probes -Dprobedir="$WORK" \
    -Dlog_level="${LOG_LEVEL:-error}" "$HERE/probes.manifest.template" probes.manifest
echo "# $(date -u +%Y-%m-%dT%H:%MZ) loader=$LOADER cpus=${SAGUARO_CPU_NUM:-1} ram=${SAGUARO_RAM_SIZE:-default} $(uptime | sed 's/.*load/load/')"
case $LOADER in
    native) timeout 300 ./probes "$@" ;;
    direct) timeout 300 gramine-direct probes "$@" ;;
    vm)     # one guest per probe (a guest panic in one probe must not lose the others)
            [ $# -gt 0 ] && [ "$1" != all ] || set -- fixed huge noreplace futexv futexrt futexstress pipeping pollmany threadlat tickgran eventfd scm unixsock unixsock2 sud signals sig_chld sig_spin sig_block sig_proc threads procs proc misc gs fs32 cs32 traps wrgsbase sig32
            for probe in "$@"; do
                t0=$(date +%s.%N)
                timeout "${PROBE_TIMEOUT:-60}" gramine-vm probes "$probe" 2>&1 | grep -v "self-check\|Frame-pool\|boot\]\|Physical\|Configuring\|REM_MEM\|vm-relay\|frame pool\|^(qemu)\|^qemu-system\|^$\|PROBES DONE"
                echo "# $probe: $(python3 -c "import time; print(round(time.time() - $t0, 1))") s with boot, exit ${PIPESTATUS[0]}"
            done ;;
esac
