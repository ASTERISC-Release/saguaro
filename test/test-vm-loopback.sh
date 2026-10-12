#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# test-vm-loopback.sh -- regression test for host-initiated connections into a guest AF_INET TCP
# server under the VM PAL (Documentation/devel/vm-network/PLAN.md, phase 1).
#
# The guest runs test/vsock-loopback/vsock_echo_server (an AF_INET TCP echo server); this script
# connects *into* it from the container with AF_VSOCK sockets: `SEQUENTIAL` sequential connections,
# then `CONCURRENT` connections established at the same time, then one more to show the listener
# survived them all; every payload must come back unchanged.
#
# Usage: test/test-vm-loopback.sh [--prefix DIR]
#   SEQUENTIAL=100  CONCURRENT=16  SIZE=8192   connection counts and payload size
#   GRAMINE_CPU_NUM=2                          vCPUs for the VM
#
# Prerequisite: a vm build installed with `scripts/install.sh --vm` (default prefix
# ~/.local/gramine-vm). Run it where /dev/kvm and /dev/vhost-vsock exist (the container).
set -u
set -o pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$HERE")
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
SEQUENTIAL=${SEQUENTIAL:-100}
CONCURRENT=${CONCURRENT:-16}
SIZE=${SIZE:-8192}
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
export GRAMINE_CPU_NUM=${GRAMINE_CPU_NUM:-2}

command -v gramine-vm >/dev/null || { echo "gramine-vm not on PATH (prefix $PREFIX?)" >&2; exit 1; }
[ -e /dev/vhost-vsock ] || { echo "no /dev/vhost-vsock; this test needs it" >&2; exit 1; }

DIR=$HERE/vsock-loopback
OUT=${OUT:-/tmp/vsock-loopback}
mkdir -p "$OUT"

echo "== vsock-loopback: building the guest server"
make -C "$DIR" -s || exit 1
# render the manifest exactly the way the libos regression suite does (gramine-test + ninja);
# `-f` forces a rerender, because `GRAMINE_LOG_LEVEL` reaches the template through the environment
# and ninja cannot see that change.
(cd "$DIR" && GRAMINE_VM=1 gramine-test build -f vsock_echo_server) >"$OUT/manifest.log" 2>&1 || {
    cat "$OUT/manifest.log"; echo "manifest build failed" >&2; exit 1; }

VM_LOG=$OUT/guest.log
rm -f "$VM_LOG"
echo "== vsock-loopback: starting the guest (RAM ${GRAMINE_RAM_SIZE:-default}, ${GRAMINE_CPU_NUM} vCPUs)"
# In relay mode, tell gramine-vm to run the per-VM vsock relay with one inbound forward
# (host TCP $RELAY_HOST_PORT -> guest 7000). The launcher starts the relay and waits until it
# listens, then boots the guest; it is stopped when the VM exits.
RELAY_HOST_PORT=${RELAY_HOST_PORT:-17000}
EXTRA_ENV=()
if [ "${RELAY:-0}" = "1" ]; then
    EXTRA_ENV=(GRAMINE_VM_INBOUND="7000:$RELAY_HOST_PORT")
    echo "== vsock-loopback: relay mode, forwarding host 127.0.0.1:$RELAY_HOST_PORT -> guest 7000"
fi
# run from the test directory so the QEMU cmdline carries the bare name `vsock_echo_server`, which
# vsock_host_client.py uses to find the guest CID of *this* VM
cd "$DIR"
env ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"} GRAMINE_VM_CONSOLE_LOG=$VM_LOG \
    gramine-vm vsock_echo_server >"$OUT/gramine-vm.out" 2>&1 &
VM_PID=$!
# cleanup note: killing `gramine-vm` does not kill its QEMU child (the launcher traps and exits),
# so also kill the QEMU of this test by its fw_cfg args signature; do not kill the process group,
# it is this script's own.
kill_guest() {
    pkill -f 'qemu-system.*-gramine-args init .vsock_echo_server' 2>/dev/null
    kill "$VM_PID" 2>/dev/null
}
trap 'kill_guest' EXIT INT TERM

echo "== vsock-loopback: $SEQUENTIAL sequential + $CONCURRENT concurrent connections, $SIZE bytes each"
rc=0
# Wait until the guest's server is listening (it prints "LISTENING 7000" once boot has reached the
# application). Direct vsock tolerates the boot itself, but in relay mode the host TCP listener is
# up before QEMU boots, so only the console line proves the guest can be reached.
for _ in $(seq 400); do
    grep -q "LISTENING" "$OUT/gramine-vm.out" 2>/dev/null && break
    kill -0 "$VM_PID" 2>/dev/null || { echo "the guest exited before listening:" >&2; \
        tail -n 20 "$OUT/gramine-vm.out" >&2; rc=1; break; }
    sleep 0.25
done
if [ $rc -eq 0 ] && ! grep -q "LISTENING" "$OUT/gramine-vm.out" 2>/dev/null; then
    echo "the guest server never reported LISTENING" >&2; rc=1
fi

CLIENT_ARGS=(--vm-pid "$VM_PID" --sequential "$SEQUENTIAL" --concurrent "$CONCURRENT"
             --size "$SIZE")
if [ "${RELAY:-0}" = "1" ]; then
    CLIENT_ARGS+=(--tcp "$RELAY_HOST_PORT")
fi
if [ $rc -eq 0 ]; then
python3 "$DIR/vsock_host_client.py" "${CLIENT_ARGS[@]}" \
    2>&1 | tee "$OUT/client.log" || rc=$?
fi

# wait for the guest to exit after GOODBYE (it prints "DONE n" and exits 0)
for _ in $(seq 100); do
    kill -0 "$VM_PID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$VM_PID" 2>/dev/null; then
    echo "the guest server is still running 10 s after GOODBYE; killing it" >&2
    kill_guest
    rc=1
fi
wait "$VM_PID" 2>/dev/null
guest_rc=$?
# the console log: gramine-vm copies it to GRAMINE_VM_CONSOLE_LOG after QEMU exits
if grep -q "DONE" "$VM_LOG" 2>/dev/null; then
    echo "== vsock-loopback: guest server handled $(grep -o 'DONE [0-9]*' "$VM_LOG" | tail -1)"
elif [ $rc -eq 0 ]; then
    echo "the guest server did not report DONE (see $VM_LOG)" >&2
    rc=1
fi
if [ "$guest_rc" != 0 ] && [ $rc -eq 0 ]; then
    echo "the guest exited with $guest_rc" >&2
    rc=1
fi

if [ $rc -eq 0 ]; then
    echo "== vsock-loopback: PASS ($(wc -l <"$OUT/client.log") lines of client output, logs in $OUT)"
else
    echo "== vsock-loopback: FAIL (logs in $OUT)"
    tail -n 40 "$VM_LOG" 2>/dev/null
fi
exit $rc
