#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run_benchmarks.sh — performance benchmarks for the direct PAL (native vs saguaro-direct).
#
# Usage:
#   ./run_benchmarks.sh                # all benchmarks
#   ./run_benchmarks.sh syscall        # per-syscall microbenchmark only
#   ./run_benchmarks.sh redis          # redis-benchmark macro benchmark only
#   RESULT=/path/out.txt ./run_benchmarks.sh   # override the results file
#
# Environment:
#   SAGUARO_PREFIX   install prefix (default: ~/.local/saguaro)
#   ITERS / REPS     syscall benchmark iterations / repetitions (default 300000 / 7)
#   REDIS_N / REDIS_C   redis-benchmark requests / clients (default 200000 / 50)
#   SERVER_CPU / CLIENT_CPUS   taskset CPU lists (default: 2 / 4-7, if that many CPUs exist)
#
# Results are appended to test/benchmarks/results/<date>_<arch>.txt (git-ignored) together with a machine
# header, so runs on different boxes (or before/after a change) can be diffed. Prerequisites:
# test/benchmarks/syscall-bench and the redis example must be buildable (test/direct/test-direct.sh builds redis).
# Re-run with the same settings before and after a change; compare min/median, not single runs.

set -uo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(dirname "$(dirname "$HERE")")
. "$ROOT/scripts/saguaro-env.sh"   # SAGUARO_PREFIX, PATH, PYTHONPATH
ITERS=${ITERS:-300000}; REPS=${REPS:-7}
REDIS_N=${REDIS_N:-200000}; REDIS_C=${REDIS_C:-50}
RESULT=${RESULT:-$HERE/results/$(date +%F)_$(uname -m).txt}
WHAT=${1:-all}

NCPU=$(nproc)
if [ "$NCPU" -ge 8 ]; then SERVER_CPU=${SERVER_CPU:-2}; CLIENT_CPUS=${CLIENT_CPUS:-4-7}
else SERVER_CPU=${SERVER_CPU:-0}; CLIENT_CPUS=${CLIENT_CPUS:-0-$((NCPU-1))}; fi

mkdir -p "$HERE/results"
say() { echo "$@" | tee -a "$RESULT"; }

header() {
    say "=============================================================="
    say "date:    $(date -Is)"
    say "git:     $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null) ($(git -C "$ROOT" status --porcelain | wc -l) modified files)"
    say "kernel:  $(uname -srm)"
    local model; model=$(lscpu | sed -n 's/^Model name:[[:space:]]*//p' | head -1)
    say "cpu:     ${model:--} x$NCPU  (vendor: $(lscpu | sed -n 's/^Vendor ID:[[:space:]]*//p' | head -1))"
    say "mem:     $(awk '/MemTotal/ {printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
    say "os:      $(. /etc/os-release; echo "$PRETTY_NAME")"
    say "gramine: direct PAL (syscall-site rewriting per the manifest default unless noted)"
    say "--------------------------------------------------------------"
}

bench_syscall() {
    say "## syscall microbenchmark (ns/call; lower is better)  ITERS=$ITERS REPS=$REPS"
    ( cd "$HERE/syscall-bench" && make -s >/dev/null ) || { say "build failed"; return 1; }
    local d="$HERE/syscall-bench"
    say "-- native";          (cd "$d" && taskset -c "$SERVER_CPU" ./syscall_bench "$ITERS" "$REPS") 2>&1 | tee -a "$RESULT"
    say "-- saguaro-direct";  (cd "$d" && taskset -c "$SERVER_CPU" saguaro-direct syscall_bench "$ITERS" "$REPS") 2>&1 | grep -v '^Emulating' | tee -a "$RESULT"
}

wait_port() { for _ in $(seq 1 120); do (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null && return 0; sleep 0.5; done; return 1; }

run_redis_one() {  # label port launcher...
    local label=$1 port=$2; shift 2
    local d="$ROOT/CI-Examples/redis"
    # setsid + exec: the pid file holds the real session leader, so kill -pid reaps the whole tree.
    ( cd "$d" && setsid bash -c 'echo $$ >"$0"; exec "$@"' "/tmp/redis-bench-$label.pid" "$@" \
        >"/tmp/redis-bench-$label.log" 2>&1 & )
    sleep 0.5
    if ! wait_port "$port"; then say "-- $label: server did not start (see /tmp/redis-bench-$label.log)"; return 1; fi
    say "-- $label"
    taskset -c "$CLIENT_CPUS" "$d/src/src/redis-benchmark" -p "$port" -q -n "$REDIS_N" -c "$REDIS_C" \
        -t set,get,incr,lpush,lpop,hset --csv 2>&1 | tee -a "$RESULT"
    say "   (pipelined, -P 16)"
    taskset -c "$CLIENT_CPUS" "$d/src/src/redis-benchmark" -p "$port" -q -n "$REDIS_N" -c "$REDIS_C" -P 16 \
        -t set,get --csv 2>&1 | tee -a "$RESULT"
    kill -KILL -"$(cat "/tmp/redis-bench-$label.pid")" 2>/dev/null; sleep 1
    pkill -KILL -x redis-server 2>/dev/null; pkill -KILL -x loader 2>/dev/null; sleep 1
}

bench_redis() {
    local d="$ROOT/CI-Examples/redis"
    [ -x "$d/redis-server" ] && [ -x "$d/src/src/redis-benchmark" ] || { say "redis not built (run test/direct/test-direct.sh redis first)"; return 1; }
    ( cd "$d" && make -s redis-server.manifest >/dev/null 2>&1 )
    say "## redis-benchmark (requests/s; higher is better)  N=$REDIS_N clients=$REDIS_C server cpu=$SERVER_CPU client cpus=$CLIENT_CPUS"
    run_redis_one native 6390 taskset -c "$SERVER_CPU" ./redis-server --port 6390 --save ""
    run_redis_one saguaro-direct 6379 taskset -c "$SERVER_CPU" saguaro-direct redis-server
}

header
case "$WHAT" in
    syscall) bench_syscall ;;
    redis)   bench_redis ;;
    all)     bench_syscall; bench_redis ;;
    *) echo "usage: $0 [syscall|redis|all]"; exit 2 ;;
esac
say "results saved to $RESULT"
