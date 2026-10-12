#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Exercise guest-initiated TCP through gramine-vm's relay, including allow and deny policies.
# The loop uses synchronous small request/reply pairs to make wakeup stalls reproducible.
set -u
set -o pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$HERE")
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
REQUESTS=${REQUESTS:-5000}
CPU_NUM=${GRAMINE_CPU_NUM:-2}
OUT=${OUT:-/tmp/vsock-outbound}

while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --help|-h) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
command -v gramine-vm >/dev/null || { echo "gramine-vm not on PATH (prefix $PREFIX?)" >&2; exit 1; }
[ -e /dev/vhost-vsock ] || { echo "no /dev/vhost-vsock; this test needs it" >&2; exit 1; }

DIR=$HERE/vsock-loopback
mkdir -p "$OUT"
make -C "$DIR" -s || exit 1

clean() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "${VM_PID:-}" ]; then
        kill "$VM_PID" 2>/dev/null || true
        wait "$VM_PID" 2>/dev/null || true
    fi
}
trap clean EXIT INT TERM

render_manifest() {
    (cd "$DIR" && GRAMINE_VM=1 GRAMINE_VM_OUTBOUND="$1" GRAMINE_VM_DENY="${2:-}" \
        gramine-test build -f vsock_outbound_client) >"$OUT/manifest.log" 2>&1 || {
        cat "$OUT/manifest.log"; return 1;
    }
}

run_guest() {
    local logfile=$1; shift
    cd "$DIR"
    GRAMINE_CPU_NUM=$CPU_NUM GRAMINE_VM_CONSOLE_LOG="$OUT/guest.console" \
        timeout --signal=TERM --kill-after=5s 120s gramine-vm vsock_outbound_client "$@" \
        >"$logfile" 2>&1
}

echo "== outbound allow: $REQUESTS synchronous small request/reply pairs, ${CPU_NUM} vCPUs"
READY=$OUT/server.port
SERVER_LOG=$OUT/server.log
rm -f "$READY" "$SERVER_LOG"
python3 "$DIR/vsock_outbound_server.py" --ready-file "$READY" --requests "$REQUESTS" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 100); do [ -s "$READY" ] && break; sleep 0.05; done
if [ ! -s "$READY" ]; then cat "$SERVER_LOG" >&2; exit 1; fi
PORT=$(cat "$READY")
render_manifest allow || exit 1
run_guest "$OUT/allow.log" 127.0.0.1 "$PORT" "$REQUESTS" || {
    cat "$OUT/allow.log" "$SERVER_LOG" >&2; exit 1;
}
wait "$SERVER_PID" || { cat "$SERVER_LOG" >&2; exit 1; }
SERVER_PID=
grep -q "OUTBOUND_DONE $REQUESTS" "$OUT/allow.log" || {
    cat "$OUT/allow.log" >&2; exit 1;
}
grep -q "ECHOED $REQUESTS" "$SERVER_LOG" || { cat "$SERVER_LOG" >&2; exit 1; }

echo "== outbound deny: vm.net.outbound=deny"
render_manifest deny || exit 1
run_guest "$OUT/deny-all.log" 127.0.0.1 "$PORT" deny || {
    cat "$OUT/deny-all.log" >&2; exit 1;
}
grep -q 'DENIED EACCES' "$OUT/deny-all.log" || { cat "$OUT/deny-all.log" >&2; exit 1; }

echo "== outbound deny rule: vm.net.deny=[tcp:$PORT]"
render_manifest allow "tcp:$PORT" || exit 1
run_guest "$OUT/deny-rule.log" 127.0.0.1 "$PORT" deny || {
    cat "$OUT/deny-rule.log" >&2; exit 1;
}
grep -q 'DENIED EACCES' "$OUT/deny-rule.log" || { cat "$OUT/deny-rule.log" >&2; exit 1; }

echo "== graphics direct port: vm.net.direct_vsock_ports=[6000]"
DIRECT_READY=$OUT/direct.ready
DIRECT_LOG=$OUT/direct.log
rm -f "$DIRECT_READY" "$DIRECT_LOG"
python3 "$DIR/vsock_direct_server.py" --ready-file "$DIRECT_READY" --requests 1000 \
    >"$DIRECT_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 100); do [ -s "$DIRECT_READY" ] && break; sleep 0.05; done
if [ ! -s "$DIRECT_READY" ]; then cat "$DIRECT_LOG" >&2; exit 1; fi
# Direct graphics ports remain available even when relayed outbound is denied.
render_manifest deny || exit 1
run_guest "$OUT/direct.log.guest" 127.0.0.1 6000 1000 || {
    cat "$OUT/direct.log.guest" "$DIRECT_LOG" >&2; exit 1;
}
wait "$SERVER_PID" || { cat "$DIRECT_LOG" >&2; exit 1; }
SERVER_PID=
grep -q 'OUTBOUND_DONE 1000' "$OUT/direct.log.guest" || {
    cat "$OUT/direct.log.guest" >&2; exit 1;
}
grep -q 'DIRECT_ECHOED 1000' "$DIRECT_LOG" || { cat "$DIRECT_LOG" >&2; exit 1; }

echo "== outbound allow/deny and direct-port path passed; logs in $OUT"
