#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# The timer-granularity measurement set of Documentation/devel/vm-timer/TESTING.md, run inside the
# development container after scripts/install.sh --vm (e.g. container/run.sh test/vm-timer/measure.sh):
# tickgran at 1 and 2 vCPUs, threadlat at 2 and 4, sleepgran.exe at 2 (if the proton-vm study's
# .proton-vm/ files are present: GE-Proton, the prefix and tests/sleepgran.exe), the LibOS suite
# under the VM at 2 vCPUs, then helloworld, spinlock and rwlock run directly with the suite's
# arguments, wall time of the whole guest. One timing run at a time: the host CPUs are shared.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
. "${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}/env.sh"
echo "## $(date -u +%Y-%m-%dT%H:%MZ) tree=$(git -c safe.directory='*' rev-parse --short HEAD 2>/dev/null) $(uptime | sed 's/.*load/load/')"
for n in 1 2; do
    echo "### tickgran ${n} vCPU"
    GRAMINE_CPU_NUM=$n test/proton-vm/run-probes.sh vm tickgran
done
for n in 2 4; do
    echo "### threadlat ${n} vCPU"
    GRAMINE_CPU_NUM=$n test/proton-vm/run-probes.sh vm threadlat
done
if [ -x "$ROOT/.proton-vm/tests/sleepgran.exe" ]; then
    echo "### sleepgran.exe 2 vCPU"
    GRAMINE_CPU_NUM=2 TIMEOUT=240 test/proton-vm/run-wine.sh vm "$ROOT/.proton-vm/tests/sleepgran.exe" 2>&1 \
        | grep "sleepgran:\|^#\|could not\|error"
fi
echo "### LibOS suite 2 vCPU (GRAMINE_VM_MULTIPROCESS unset)"
t0=$(date +%s)
GRAMINE_CPU_NUM=2 test/test-vm.sh libos 2>&1 | grep -v "^$" | tail -45
echo "suite wall $(( $(date +%s) - t0 )) s"
echo "### helloworld, spinlock, rwlock (2 vCPU)"
cd libos/test/regression
FILTER="self-check\|Frame-pool\|boot\]\|Physical\|Configuring\|REM_MEM\|vm-relay\|frame pool\|^(qemu)\|^qemu-system\|^$"
for t in helloworld spinlock; do
    TIMEFORMAT="$t wall %R s"
    time ( GRAMINE_VM=1 GRAMINE_CPU_NUM=2 timeout 120 gramine-vm $t 2>&1 | grep -v "$FILTER" | tail -3 )
done
TIMEFORMAT="rwlock 5 100 10 3 100 wall %R s"
time ( GRAMINE_VM=1 GRAMINE_CPU_NUM=2 timeout 300 gramine-vm rwlock 5 100 10 3 100 2>&1 | grep -v "$FILTER" | tail -3 )
echo "## done $(date -u +%Y-%m-%dT%H:%MZ) $(uptime | sed 's/.*load/load/')"
