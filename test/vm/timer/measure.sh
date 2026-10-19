#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# The timer-granularity measurement set of Documentation/devel/vm-timer/TESTING.md, for the
# aarch64 VM PAL, run inside the development container after scripts/install.sh --vm (for
# example container/run.sh test/vm-timer/measure.sh): tickgran (1 ms and 5 ms nanosleep, futex,
# poll and epoll_wait waits) at 1 and 2 vCPUs, threadlat and pipeping at 2. The LibOS suite is
# run separately (Documentation/devel/aarch64/vm/TESTING.md). GRAMINE_PREFIX selects the install,
# RUNS the number of repetitions (default 3), TICK_VCPUS (default "1 2") and LAT_VCPUS (default 2) the vCPU counts. One timing run at a time: the host CPUs are shared.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
export GRAMINE_PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
# probes.c is x86-only; on aarch64 the three timer probes alone build
if [ "$(uname -m)" = aarch64 ]; then
    export PROBES_SRC=${PROBES_SRC:-$ROOT/test/vm-timer/timerprobes.c}
fi
echo "## $(date -u +%Y-%m-%dT%H:%MZ) tree=$(git -c safe.directory='*' rev-parse --short HEAD 2>/dev/null) prefix=$GRAMINE_PREFIX $(uptime | sed 's/.*load/load/')"
for run in $(seq 1 "${RUNS:-3}"); do
    for n in ${TICK_VCPUS:-1 2}; do
        echo "### run $run tickgran ${n} vCPU"
        GRAMINE_CPU_NUM=$n test/proton-vm/run-probes.sh vm tickgran
    done
    echo "### run $run threadlat ${LAT_VCPUS:-2} vCPU"
    GRAMINE_CPU_NUM=${LAT_VCPUS:-2} test/proton-vm/run-probes.sh vm threadlat
    echo "### run $run pipeping ${LAT_VCPUS:-2} vCPU"
    GRAMINE_CPU_NUM=${LAT_VCPUS:-2} test/proton-vm/run-probes.sh vm pipeping
done
echo "## done $(date -u +%Y-%m-%dT%H:%MZ) $(uptime | sed 's/.*load/load/')"
