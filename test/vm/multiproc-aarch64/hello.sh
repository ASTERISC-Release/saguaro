#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# hello.sh -- CI-Examples/helloworld under the AArch64 saguaro-vm with the multiprocess key on and
# off, at each vCPU count given (default 1 2 4). With the key on the boot runs the frame-pool,
# address-space and scheduler self-checks (arch/aarch64/pal_main.c). Prints one line per run and
# keeps every console log (and the PL011 serial log) in $OUT.
#
# Usage: test/vm/multiproc-aarch64/hello.sh [--prefix DIR] [--key on|off|both] [vcpus...]
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
KEY=both
CPUS=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --key) KEY=$2; shift 2 ;;
        *) CPUS+=("$1"); shift ;;
    esac
done
[ ${#CPUS[@]} -gt 0 ] || CPUS=(1 2 4)
export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
export SAGUARO_PREFIX=$PREFIX
OUT=${OUT:-/tmp/mp-hello}
mkdir -p "$OUT"

(cd "$REPO/CI-Examples/helloworld" && make -B helloworld.manifest >/dev/null 2>&1 && make >/dev/null 2>&1) \
    || { echo "cannot build CI-Examples/helloworld"; exit 2; }
W=$OUT/work
rm -rf "$W" && mkdir -p "$W"
cp "$REPO/CI-Examples/helloworld/helloworld" "$REPO/CI-Examples/helloworld/helloworld.manifest" "$W/"
# top-level key: before the first table of the rendered manifest
{ echo 'sys.experimental__multiprocess = true'; cat "$W/helloworld.manifest"; } > "$W/helloworld-mp.manifest"
ln -sf helloworld "$W/helloworld-mp"

keys=()
case $KEY in on) keys=(mp) ;; off) keys=(base) ;; *) keys=(base mp) ;; esac
rc=0
for n in "${CPUS[@]}"; do
    for k in "${keys[@]}"; do
        app=helloworld; [ $k = mp ] && app=helloworld-mp
        log=$OUT/hello-$k-cpu$n.log
        load=$(cut -d' ' -f1 /proc/loadavg)
        start=$(date +%s%N)
        (cd "$W" && SAGUARO_CPU_NUM=$n SAGUARO_VM_SERIAL=$OUT/hello-$k-cpu$n.serial \
            timeout ${RUN_TIMEOUT:-120} saguaro-vm $app > "$log" 2>&1)
        r=$?
        t=$(( ($(date +%s%N) - start) / 1000000 ))
        if [ $r = 0 ] && grep -q "Hello, world" "$log"; then v=PASS; else v="FAIL(rc=$r)"; rc=1; fi
        echo "$v key=$k vcpus=$n time=${t}ms load1=$load $(grep -c 'self-check passed' "$log") self-checks; $(grep -o 'Address-space switch: [0-9]* ns' "$log")"
    done
done
exit $rc
