#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# One measurement of the SEV-SNP timing set of Documentation/devel/vm-timer/TESTING.md, run in the
# development container after scripts/install.sh --sev (gramine-sev, SNP mode):
#
#   measure-sev.sh libos  N OUTDIR   LibOS suite at N vCPUs, pytest --durations=0 and --junitxml
#   measure-sev.sh probes N OUTDIR   tickgran (1 ms and 5 ms waits) and threadlat (pipe round trip)
#   measure-sev.sh idle   N OUTDIR [PERIOD_MS]
#                                    KVM interrupt injections of an idle guest over 30 s (QMP
#                                    query-stats), the guest sleeping in one wait (PERIOD_MS 0) or
#                                    in PERIOD_MS steps
#
# The tree is the checkout this script is run from; TREE=<dir> overrides (the baseline tree has no
# test/vm-timer/ files, so the same script and idle.c are run against its install). Needs
# QEMU_PATH (default /root/sev-tools/prefix/bin/qemu-system-x86_64) and /dev/sev. Output files:
# OUTDIR/{libos,probes}-N.* and idle-N-pPERIOD.* . One run at a time: the host CPUs are shared.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../.." && pwd)}
KIND=${1:?kind}; N=${2:?vcpus}; OUT=${3:?outdir}; PERIOD=${4:-0}
mkdir -p "$OUT"
. "${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}/env.sh"
export QEMU_PATH=${QEMU_PATH:-/root/sev-tools/prefix/bin/qemu-system-x86_64}
export GRAMINE_SEV_MODE=snp GRAMINE_RAM_SIZE=${GRAMINE_RAM_SIZE:-4G} GRAMINE_CPU_NUM=$N
echo "## $(date -u +%Y-%m-%dT%H:%M:%SZ) $KIND vcpus=$N tree=$(git -c safe.directory='*' -C "$TREE" rev-parse --short HEAD) $(uptime | sed 's/.*load/load/')"

# a manifest for a program in $W, from the proton-vm probes template without the multi-process key
# (the frame pool of that key does not fit in 4G)
prog_manifest() {  # <workdir> <program>
    mkdir -p "$1/tmp"
    ( cd "$1" && gramine-manifest -Darch_libdir=/lib/x86_64-linux-gnu -Dentry=/probe/$2 -Dprobedir="$1" \
        -Dlog_level=error "$TREE/test/proton-vm/probes.manifest.template" $2.manifest &&
      sed -i '/experimental__multiprocess/d' $2.manifest )
}
FILTER="self-check\|Frame-pool\|boot\]\|Physical\|Configuring\|REM_MEM\|vm-relay\|frame pool\|^(qemu)\|^qemu-system\|^$\|PROBES DONE\|launch digest"

case $KIND in
libos)
    cd "$TREE/libos/test/regression" || exit 2
    GRAMINE_VM=1 GRAMINE_VM_LAUNCHER=gramine-sev gramine-test pytest -- -q -rfEsxX --tb=short \
        -p no:cacheprovider --durations=0 --junitxml="$OUT/libos-$N.xml" test_libos.py \
        > "$OUT/libos-$N.log" 2>&1
    echo "exit $?"; grep -E "[0-9]+ (passed|failed)" "$OUT/libos-$N.log" | tail -n 1 ;;
probes)
    W=/tmp/probes-w; prog_manifest $W probes
    gcc -O1 -g -pthread -fno-stack-protector -o $W/probes "$TREE/test/proton-vm/probes.c" 2>/dev/null
    cd $W
    for p in tickgran threadlat; do
        [ "$p" = threadlat ] && [ "$N" = 1 ] && continue
        echo "# $p"
        timeout 120 gramine-sev probes $p 2>&1 | grep -v "$FILTER"
    done | tee "$OUT/probes-$N.log" ;;
idle)
    W=/tmp/idle-w; prog_manifest $W idle
    gcc -O1 -o $W/idle "$HERE/idle.c" || exit 2
    cd $W
    Q=$OUT/idle-$N-p$PERIOD.qmp; rm -f "$Q"
    GRAMINE_VM_QMP=$Q gramine-sev idle 50 $PERIOD > $OUT/idle-$N-p$PERIOD.guest 2>&1 &
    sleep 12; python3 "$HERE/qstats.py" "$Q" > $OUT/idle-$N-p$PERIOD.a.json
    sleep 30; python3 "$HERE/qstats.py" "$Q" > $OUT/idle-$N-p$PERIOD.b.json
    wait
    python3 "$HERE/idle-delta.py" $OUT/idle-$N-p$PERIOD.a.json $OUT/idle-$N-p$PERIOD.b.json | tee "$OUT/idle-$N-p$PERIOD.txt" ;;
*) echo "unknown kind $KIND" >&2; exit 2 ;;
esac
