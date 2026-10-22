#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Redis smoke test for the VM's static inbound relay.
set -u
set -o pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)
PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
GUEST_PORT=${REDIS_GUEST_PORT:-6379}
HOST_PORT=${REDIS_HOST_PORT:-16379}
OUT=${OUT:-/tmp/vm-redis}
mkdir -p "$OUT"

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)

command -v saguaro-vm >/dev/null || { echo "saguaro-vm not on PATH (prefix $PREFIX?)" >&2; exit 1; }

python3 - "$HOST_PORT" <<'PY' || { echo "host port $HOST_PORT is busy" >&2; exit 1; }
import socket
import sys
s = socket.socket()
s.bind(('127.0.0.1', int(sys.argv[1])))
s.close()
PY

APP=$REPO/CI-Examples/redis
(cd "$APP" && make) >"$OUT/build.log" 2>&1 || {
    tail -n 30 "$OUT/build.log" >&2
    exit 1
}
REDIS_CLI=$APP/src/src/redis-cli
REDIS_BENCHMARK=$APP/src/src/redis-benchmark
[ -x "$REDIS_CLI" ] && [ -x "$REDIS_BENCHMARK" ] || {
    echo "Redis client binaries are missing; check $OUT/build.log" >&2
    exit 1
}

VM_LOG=$OUT/saguaro-vm.log
rm -f "$VM_LOG"
cd "$APP"
SAGUARO_VM_INBOUND="$GUEST_PORT:$HOST_PORT" SAGUARO_VM_CONSOLE_LOG=$VM_LOG \
    saguaro-vm redis-server >"$OUT/saguaro-vm.out" 2>&1 &
VM_PID=$!

stop_vm() {
    # Signal only QEMU children of this launch; do not pattern-kill shared guests.
    python3 - "$VM_PID" <<'PY'
import os
import signal
import sys

ancestor = int(sys.argv[1])
parents = {}
for entry in os.listdir('/proc'):
    if not entry.isdigit():
        continue
    try:
        with open(f'/proc/{entry}/stat', 'rb') as f:
            parents[int(entry)] = int(f.read().rsplit(b')', 1)[1].split()[1])
    except (OSError, IndexError, ValueError):
        pass

children = {}
for pid, parent in parents.items():
    children.setdefault(parent, []).append(pid)
todo = [ancestor]
while todo:
    parent = todo.pop()
    for pid in children.get(parent, []):
        todo.append(pid)
        try:
            with open(f'/proc/{pid}/cmdline', 'rb') as f:
                argv = f.read().split(b'\0')
            if argv and b'qemu-system' in argv[0]:
                os.kill(pid, signal.SIGTERM)
        except (OSError, ProcessLookupError):
            pass
PY
    kill "$VM_PID" 2>/dev/null || true
    wait "$VM_PID" 2>/dev/null || true
}
trap stop_vm EXIT INT TERM

ready=0
for _ in $(seq 240); do
    kill -0 "$VM_PID" 2>/dev/null || { tail -n 40 "$OUT/saguaro-vm.out" >&2; exit 1; }
    if [ "$("$REDIS_CLI" -h 127.0.0.1 -p "$HOST_PORT" ping 2>/dev/null)" = PONG ]; then
        ready=1
        break
    fi
    sleep 0.25
done
if [ "$ready" != 1 ]; then
    echo "Redis did not answer PING through the VM relay" >&2
    tail -n 40 "$OUT/saguaro-vm.out" >&2
    exit 1
fi

key=saguaro-vm-relay-smoke
value=phase2-inbound-ok
[ "$("$REDIS_CLI" -h 127.0.0.1 -p "$HOST_PORT" set "$key" "$value")" = OK ] || exit 1
[ "$("$REDIS_CLI" -h 127.0.0.1 -p "$HOST_PORT" get "$key")" = "$value" ] || exit 1
"$REDIS_BENCHMARK" -q -h 127.0.0.1 -p "$HOST_PORT" -c 10 -n 5000 -P 1 -t set,get \
    | tee "$OUT/benchmark.log"
echo "Redis PING/SET/GET and benchmark passed through saguaro-vm relay (host port $HOST_PORT)."
