#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Run CI-Examples applications under `gramine-vm` on aarch64 (inside the saguaro-vm-aarch64 container,
# with the VM build installed; see dev docs: vm.md) and check their output.
#
# Usage: tools/run-vm-aarch64-examples.sh [example ...]        (default: all below)
#   SAGUARO_CPU_NUM=2   vCPUs per run          RUN_TIMEOUT=120   per-run timeout (s)
#   VM_PREFIX=/tmp/vm-prefix                   OUT=/tmp/vm-examples (per-run logs)
#
# Each example's manifest is re-rendered with `make -B` against the VM install (so loader.entrypoint
# is the VM libsysdb), then run with `gramine-vm`. On aarch64 the application binary must be PIE (the
# VM's DRAM starts at 0x40000000, a non-PIE binary linked at 0x400000 cannot be mapped); non-PIE ones
# are reported SKIP. x86_64 has no such restriction. Examples that need a TCP/IP stack (redis, memcached, nginx, lighttpd, rust-hyper,
# networks) cannot run: the VM PAL only has vsock.
set -u

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VM_PREFIX=${VM_PREFIX:-/tmp/vm-prefix}
export PATH=$VM_PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$VM_PREFIX"/lib/python3*/site-packages | head -1)
export SAGUARO_CPU_NUM=${SAGUARO_CPU_NUM:-2}
RUN_TIMEOUT=${RUN_TIMEOUT:-120}
OUT=${OUT:-/tmp/vm-examples}
mkdir -p "$OUT"

pass=0; fail=0; skip=0; failed=""

# run_example <name> <dir> <manifest-target> <binary> <expected-substring> <stdin-file|-> <app> [args...]
run_example() {
    local name=$1 dir=$2 target=$3 binary=$4 expect=$5 stdin=$6 app=$7; shift 7
    local log="$OUT/$name.log"
    cd "$ROOT/CI-Examples/$dir" || { echo "SKIP  $name (no dir)"; skip=$((skip+1)); return; }
    if ! make -B "$target" > "$OUT/$name.make.log" 2>&1; then
        echo "SKIP  $name (make $target failed, see $OUT/$name.make.log)"; skip=$((skip+1)); return
    fi
    # a fresh checkout has no built application binary yet
    if [ -n "$binary" ] && [ ! -e "$binary" ]; then
        make "$binary" > "$OUT/$name.build.log" 2>&1 || true
    fi
    # only aarch64 needs PIE (its guest DRAM starts at 0x40000000); on x86_64 a non-PIE binary maps
    if [ "$(uname -m)" = aarch64 ] && [ -n "$binary" ] && [ -e "$binary" ] \
            && ! file -L "$binary" | grep -q "pie executable\|shared object"; then
        echo "SKIP  $name ($binary is not PIE)"; skip=$((skip+1)); return
    fi
    if [ "$stdin" = - ]; then
        timeout "$RUN_TIMEOUT" gramine-vm "$app" "$@" > "$log" 2>&1 < /dev/null; rc=$?
    else
        timeout "$RUN_TIMEOUT" gramine-vm "$app" "$@" > "$log" 2>&1 < "$stdin"; rc=$?
    fi
    if [ $rc = 124 ]; then verdict=HANG
    elif grep -q "Panic" "$log"; then verdict=PANIC
    elif [ $rc != 0 ]; then verdict="FAIL(exit=$rc)"
    elif [ -n "$expect" ] && ! grep -qF -- "$expect" "$log"; then verdict="FAIL(output)"
    else verdict=PASS; fi
    printf '%-6s %s\n' "${verdict%%(*}" "$name"
    if [ "$verdict" = PASS ]; then pass=$((pass+1)); else
        fail=$((fail+1)); failed="$failed $name"; printf '       -> %s  (%s)\n' "$verdict" "$log"
    fi
}

ALL="helloworld mremap multi-thread bash busybox python sqlite"
SEL=${*:-$ALL}
for ex in $SEL; do
    case $ex in
    helloworld)
        run_example helloworld helloworld helloworld.manifest helloworld "Hello, world" - helloworld ;;
    mremap)
        run_example mremap unit_tests/mremap helloworld.manifest helloworld "ALL IN-PLACE TESTS PASSED" - helloworld ;;
    multi-thread)
        # the example links thread-tls with plain -static (non-PIE) and thread-rwlock mounts the
        # thread-stress binary too; the VM needs static-pie for all of them
        (cd "$ROOT/CI-Examples/unit_tests/multi-thread" && for b in thread-tls thread-stress; do
            gcc -static-pie -fPIE -O2 -pthread -o $b $b.c; done) > "$OUT/thread-tls.build.log" 2>&1
        run_example thread-rwlock unit_tests/multi-thread thread-rwlock.manifest thread-rwlock "" - thread-rwlock
        run_example thread-tls unit_tests/multi-thread thread-tls.manifest thread-tls "" - thread-tls ;;
    bash)
        run_example bash-echo bash bash.manifest bash "gramine-vm-ok" - bash -c "echo gramine-vm-ok"
        run_example bash-ls bash bash.manifest bash "Makefile" - bash -c "ls" ;;
    busybox)
        run_example busybox-echo busybox busybox.manifest busybox "Hello" - busybox echo Hello
        run_example busybox-uname busybox busybox.manifest busybox "$(uname -m)" - busybox uname -m ;;
    python)
        # Ubuntu's python3 is a non-PIE ET_EXEC (linked at 0x400000): skipped on aarch64 unless a PIE one is used
        run_example python python python.manifest "$(readlink -f "$(command -v python3)")" "42" - python -c "print(6*7)" ;;
    sqlite)
        run_example sqlite sqlite sqlite3.manifest "" "" scripts/create.sql sqlite3 ;;
    *) echo "unknown example: $ex" ;;
    esac
done
echo "== pass=$pass fail=$fail skip=$skip${failed:+ failed:$failed}"
[ "$fail" = 0 ]
