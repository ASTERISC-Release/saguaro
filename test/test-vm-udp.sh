#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Test guest getaddrinfo through the host resolver and one guest-to-host UDP echo datagram.
set -u
set -o pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
DNS_NAME=${DNS_NAME:-example.com}
CPU_NUM=${GRAMINE_CPU_NUM:-2}
OUT=${OUT:-/tmp/vsock-udp}

while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --help|-h) sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
command -v gramine-vm >/dev/null || { echo "gramine-vm not on PATH (prefix $PREFIX?)" >&2; exit 1; }
[ -e /dev/vhost-vsock ] || { echo "no /dev/vhost-vsock; this test needs it" >&2; exit 1; }
test_host_ip() {
    python3 -c 'import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(("1.1.1.1", 53)); print(s.getsockname()[0]); s.close()'
}
HOST_IP=${GRAMINE_VM_TEST_HOST_IP:-$(test_host_ip)}
case $HOST_IP in 127.*|0.0.0.0) echo "not a routable container address: $HOST_IP" >&2; exit 1 ;; esac

DIR=$HERE/vsock-loopback
mkdir -p "$OUT"
make -C "$DIR" -s || exit 1
(cd "$DIR" && GRAMINE_VM=1 gramine-test build -f vsock_udp_client) \
    >"$OUT/manifest.log" 2>&1 || { cat "$OUT/manifest.log"; exit 1; }

SERVER_PID=
clean() {
    if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; fi
}
trap clean EXIT INT TERM

READY=$OUT/server.port
SERVER_LOG=$OUT/server.log
rm -f "$READY" "$SERVER_LOG"
python3 "$DIR/vsock_udp_server.py" --ready-file "$READY" --bind-address "$HOST_IP" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 100); do [ -s "$READY" ] && break; sleep 0.05; done
if [ ! -s "$READY" ]; then cat "$SERVER_LOG" >&2; exit 1; fi
PORT=$(cat "$READY")

echo "== UDP echo to $HOST_IP and DNS $DNS_NAME, ${CPU_NUM} vCPUs"
cd "$DIR"
GRAMINE_CPU_NUM=$CPU_NUM GRAMINE_VM_CONSOLE_LOG="$OUT/guest.console" \
    timeout --signal=TERM --kill-after=5s 120s gramine-vm vsock_udp_client \
    "$DNS_NAME" "$HOST_IP" "$PORT" >"$OUT/guest.log" 2>&1 || {
        cat "$OUT/guest.log" "$SERVER_LOG" >&2; exit 1;
    }
wait "$SERVER_PID" || { cat "$SERVER_LOG" >&2; exit 1; }
SERVER_PID=
grep -q "DNS_RESOLVED $DNS_NAME " "$OUT/guest.log" || { cat "$OUT/guest.log" >&2; exit 1; }
grep -q UDP_ECHO_OK "$OUT/guest.log" || { cat "$OUT/guest.log" >&2; exit 1; }
grep -q 'UDP_ECHOED ' "$SERVER_LOG" || { cat "$SERVER_LOG" >&2; exit 1; }
echo "== UDP/DNS passed; logs in $OUT"
