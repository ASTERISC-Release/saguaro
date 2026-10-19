#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# test-sev-all.sh -- the qualification run of the AMD SEV build of the vm PAL (Documentation/devel/
# sev/PLAN.md, phase 5), for a machine with SEV-SNP (there are no hosted CI runners with SEV). Run it
# in the container of container/run.sh, after container/sev/build-sev-tools.sh all.
#
#   1. scripts/install.sh --sev (unless NO_BUILD=1)
#   2. test/test-sev.sh under plain SEV and under SNP (hello, memory, vsock, measure / attest)
#   3. the LibOS regression suite under SNP at the vCPU counts of CPUS (default "1 2 4"); reported,
#      not judged: compare with gramine-vm on the same host (known failures on the development host:
#      test_020_cpuinfo on AMD, and at 1 vCPU test_000_spinlock and test_074_eventfd_races, as with
#      gramine-vm)
#   4. CI-Examples under SNP: helloworld, the multi-threaded unit test, python
#   5. the multi-process checks under SNP (test/vm-multiproc/check-probes.sh, MP_RAM default 8G),
#      unless MP=0
#
# Logs go to OUT (default /tmp/test-sev-all); exit status 0 only if every step passed.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$HERE")
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
OUT=${OUT:-/tmp/test-sev-all}
CPUS=${CPUS-"1 2 4"}   # CPUS= (empty) skips the suite
export GRAMINE_RAM_SIZE=${GRAMINE_RAM_SIZE:-8G}
mkdir -p "$OUT"
rc=0
step() { echo "=== $* ($(date -u +%H:%M:%S), $(cut -d' ' -f1-3 /proc/loadavg))"; }
fail() { echo "FAIL: $*"; rc=1; }

cd "$REPO" || exit 2
if [ "${NO_BUILD:-0}" != 1 ]; then
    step "build"
    scripts/install.sh --sev --graphics disabled --prefix "$PREFIX" > "$OUT/build.log" 2>&1 \
        || { tail -n 30 "$OUT/build.log"; echo "FAIL: build"; exit 1; }
fi
. "$PREFIX/env.sh"

for mode in sev snp; do
    step "test-sev.sh, $mode"
    GRAMINE_SEV_MODE=$mode OUT=$OUT/test-sev-$mode "$HERE/test-sev.sh" --prefix "$PREFIX" \
        > "$OUT/test-sev-$mode.log" 2>&1 || fail "test-sev.sh $mode"
    tail -n 1 "$OUT/test-sev-$mode.log"
done

for n in $CPUS; do
    step "LibOS suite under SNP, $n vCPUs"
    GRAMINE_CPU_NUM=$n GRAMINE_VM_LAUNCHER=gramine-sev GRAMINE_SEV_MODE=snp \
        "$HERE/test-vm.sh" --prefix "$PREFIX" libos > "$OUT/libos-snp-$n.log" 2>&1
    grep -E "^FAILED|[0-9]+ (passed|failed)" "$OUT/libos-snp-$n.log" | tail -n 20
done

step "CI-Examples under SNP"
export GRAMINE_SEV_MODE=snp
( cd CI-Examples/helloworld && make -s >/dev/null && timeout 300 gramine-sev helloworld ) \
    > "$OUT/ex-helloworld.log" 2>&1
grep -q "Hello, world" "$OUT/ex-helloworld.log" && echo "helloworld: ok" || fail "helloworld"
( cd CI-Examples/unit_tests/multi-thread && make -s thread-tls thread-tls.manifest >/dev/null &&
  GRAMINE_CPU_NUM=4 timeout 300 gramine-sev thread-tls ) > "$OUT/ex-threads.log" 2>&1
grep -q "(again)" "$OUT/ex-threads.log" && echo "thread-tls (4 vCPUs): ok" || fail "thread-tls"
( cd CI-Examples/python && make -s python.manifest >/dev/null &&
  timeout 600 gramine-sev python scripts/helloworld.py ) > "$OUT/ex-python.log" 2>&1
grep -q "Hello World" "$OUT/ex-python.log" && echo "python helloworld: ok" || fail "python"

if [ "${MP:-1}" != 0 ]; then
    step "multi-process checks under SNP"
    # the frame pool needs a free GB of RAM above 4G: 4G guests have none (as with gramine-vm)
    GRAMINE_RAM_SIZE=${MP_RAM:-8G} GRAMINE_VM_LAUNCHER=gramine-sev WORK=$OUT/mp \
        "$HERE/vm-multiproc/check-probes.sh" \
        > "$OUT/multiproc.log" 2>&1 || fail "multi-process checks"
    tail -n 12 "$OUT/multiproc.log"
fi

step "done"
[ $rc -eq 0 ] && echo "test-sev-all: PASS (logs in $OUT)" || echo "test-sev-all: FAIL (logs in $OUT)"
exit $rc
