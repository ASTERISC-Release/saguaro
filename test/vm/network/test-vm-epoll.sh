#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Many synchronous small request/reply exchanges through the VM PAL's epoll readiness path.
set -u
set -o pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
REQUESTS=${EPOLL_REQUESTS:-50000}
SERVER_MODE=${EPOLL_SERVER_MODE:-epoll}
TRANSPORT=${EPOLL_TRANSPORT:-vsock}
GUEST_PORT=${EPOLL_GUEST_PORT:-7001}
HOST_PORT=${EPOLL_HOST_PORT:-17001}
OUT=${OUT:-/tmp/vsock-epoll}
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT"

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
export SAGUARO_CPU_NUM=${SAGUARO_CPU_NUM:-2}

command -v saguaro-vm >/dev/null || { echo "saguaro-vm not on PATH (prefix $PREFIX?)" >&2; exit 1; }
[ -e /dev/vhost-vsock ] || { echo "no /dev/vhost-vsock; this test needs it" >&2; exit 1; }
if [ "$TRANSPORT" = tcp ]; then
python3 - "$HOST_PORT" <<'PY' || { echo "host port $HOST_PORT is busy" >&2; exit 1; }
import socket
import sys
s = socket.socket()
s.bind(('127.0.0.1', int(sys.argv[1])))
s.close()
PY
elif [ "$TRANSPORT" != vsock ]; then
    echo "unknown EPOLL_TRANSPORT=$TRANSPORT (expected tcp or vsock)" >&2
    exit 2
fi

DIR=$(cd "$HERE/../vsock-loopback" && pwd)
echo "== vsock-epoll: building guest echo server"
make -C "$DIR" -s || exit 1
(cd "$DIR" && SAGUARO_VM=1 gramine-test build -f vsock_echo_server) >"$OUT/manifest.log" 2>&1 || {
    cat "$OUT/manifest.log"; echo "manifest build failed" >&2; exit 1; }

VM_LOG=$OUT/saguaro-vm.log
rm -f "$VM_LOG"
echo "== vsock-epoll: starting ${SAGUARO_CPU_NUM}-vCPU guest in $SERVER_MODE mode, $REQUESTS synchronous requests"
cd "$DIR"
if [ "$TRANSPORT" = tcp ]; then
    SAGUARO_VM_INBOUND="$GUEST_PORT:$HOST_PORT" SAGUARO_VM_RELAY_DEBUG=1 \
        SAGUARO_VM_CONSOLE_LOG=$VM_LOG \
        saguaro-vm vsock_echo_server "$GUEST_PORT" "$SERVER_MODE" "$REQUESTS" \
        >"$OUT/saguaro-vm.out" 2>&1 &
else
    SAGUARO_VM_CONSOLE_LOG=$VM_LOG saguaro-vm vsock_echo_server "$GUEST_PORT" "$SERVER_MODE" \
        "$REQUESTS" >"$OUT/saguaro-vm.out" 2>&1 &
fi
VM_PID=$!

kill_guest() {
    # Find and signal only this launcher's QEMU child by PID; do not pattern-kill shared guests.
    python3 - "$VM_PID" <<'PY'
import os
import signal
import sys

ancestor = int(sys.argv[1])
def ppid(pid):
    try:
        with open(f'/proc/{pid}/stat', 'rb') as f:
            return int(f.read().rsplit(b')', 1)[1].split()[1])
    except (OSError, IndexError, ValueError):
        return 0

for entry in os.listdir('/proc'):
    if not entry.isdigit():
        continue
    try:
        with open(f'/proc/{entry}/cmdline', 'rb') as f:
            argv = f.read().split(b'\0')
    except OSError:
        continue
    if not argv or b'qemu' not in argv[0] or not any(b'vsock_echo_server' in arg for arg in argv):
        continue
    pid = int(entry)
    parent = ppid(pid)
    while parent and parent != 1 and parent != ancestor:
        parent = ppid(parent)
    if parent == ancestor:
        os.kill(pid, signal.SIGTERM)
PY
    kill "$VM_PID" 2>/dev/null
    wait "$VM_PID" 2>/dev/null
}
trap kill_guest EXIT INT TERM

for _ in $(seq 400); do
    grep -q "LISTENING" "$OUT/saguaro-vm.out" 2>/dev/null && break
    kill -0 "$VM_PID" 2>/dev/null || { tail -n 30 "$OUT/saguaro-vm.out" >&2; exit 1; }
    sleep 0.25
done
if ! grep -q "LISTENING" "$OUT/saguaro-vm.out" 2>/dev/null; then
    echo "guest epoll server never reported LISTENING" >&2
    exit 1
fi

CLIENT_PORT=$HOST_PORT
if [ "$TRANSPORT" = vsock ]; then
    CLIENT_PORT=$GUEST_PORT
    export EPOLL_CID=$(PYTHONPATH="$DIR" python3 - "$VM_PID" <<'PY'
import sys
from vsock_host_client import find_child_guest_cid
print(find_child_guest_cid('vsock_echo_server', sys.argv[1]))
PY
    )
fi
EPOLL_TRANSPORT=$TRANSPORT timeout 30 python3 "$DIR/vsock_epoll_client.py" "$CLIENT_PORT" "$REQUESTS" \
    2>&1 | tee "$OUT/client.log"
rc=${PIPESTATUS[0]}
if [ "$rc" -ne 0 ]; then
    # Let the guest consume the peer shutdown and flush its diagnostic progress before cleanup.
    sleep "${EPOLL_FAILURE_GRACE:-0.5}"
    echo "== vsock-epoll: FAIL (logs in $OUT)" >&2
    exit "$rc"
fi

wait "$VM_PID"
VM_PID=
trap - EXIT INT TERM
if ! grep -q "DONE $REQUESTS" "$VM_LOG" "$OUT/saguaro-vm.out" 2>/dev/null; then
    echo "guest did not report DONE $REQUESTS (logs in $OUT)" >&2
    exit 1
fi
echo "== vsock-epoll: PASS"
