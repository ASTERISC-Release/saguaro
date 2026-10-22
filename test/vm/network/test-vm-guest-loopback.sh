#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Test guest TCP and UDP loopback on IPv4 and IPv6 through the per-VM relay.
set -u
set -o pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
CPU_NUM=${SAGUARO_CPU_NUM:-2}
OUT=${OUT:-/tmp/vsock-guest-loopback}

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

DIR=$(cd "$HERE/../vsock-loopback" && pwd)
mkdir -p "$OUT"
make -C "$DIR" -s || exit 1
(cd "$DIR" && SAGUARO_VM=1 gramine-test build -f vsock_guest_loopback) \
    >"$OUT/manifest.log" 2>&1 || { cat "$OUT/manifest.log"; exit 1; }

cd "$DIR"
echo "== guest loopback: TCP echo, async connect and socket names, UDP self-echo, ${CPU_NUM} vCPUs"
SAGUARO_CPU_NUM=$CPU_NUM SAGUARO_VM_CONSOLE_LOG="$OUT/guest.console" \
    timeout --signal=TERM --kill-after=5s 120s gramine-vm vsock_guest_loopback \
    >"$OUT/guest.log" 2>&1 || { cat "$OUT/guest.log" >&2; exit 1; }
grep -q 'GUEST_LOOPBACK_OK tcp4=1000 tcp6=1000 nonblock4=1 nonblock6=1 udp4=1 udp6=1 rcvtimeo=2' "$OUT/guest.log" || {
    cat "$OUT/guest.log" >&2; exit 1;
}
echo "== guest loopback passed; logs in $OUT"
