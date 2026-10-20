#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): the iperf3 server runs under $GRAMINE (its manifest sets the server
# mode) and the native iperf3 client of the same build measures it over the loopback of the host.
# Under the vm and sev the port is forwarded to the guest through the relay (GRAMINE_VM_INBOUND).
#
# The script does not probe the port first: iperf3 counts a connection that closes without a test as an
# error. The client is retried instead, until the server is up.
set -uo pipefail
: "${GRAMINE:=gramine-direct}"
PORT=${IPERF_PORT:-5201}
SECONDS_=${IPERF_SECONDS:-2}
TRIES=${IPERF_TRIES:-60}

export GRAMINE_VM_INBOUND="$PORT:$PORT"
# timeout runs the server in its own process group and passes a signal on to all of it, so stopping the
# server also stops the QEMU process of the vm and sev (killing only gramine-vm leaves it running)
timeout 600 "$GRAMINE" iperf3 > server.log 2>&1 </dev/null &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f server.log client.log' EXIT

for i in $(seq "$TRIES"); do
    LD_LIBRARY_PATH=./install timeout 30 ./install/iperf3 -c 127.0.0.1 -p "$PORT" -t "$SECONDS_" \
        > client.log 2>&1 </dev/null
    grep -q "receiver" client.log && break
    kill -0 $pid 2>/dev/null || { echo "FAIL: the server exited"; cat server.log; exit 1; }
    sleep 1
done
cat client.log
grep -q "receiver" client.log || { echo "FAIL: no receiver summary after $TRIES tries"; tail -n 20 server.log; exit 1; }
echo "[ Success 1/1 ]"
