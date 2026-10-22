#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): the iperf3 server runs under $SAGUARO (its manifest sets the server
# mode) and the native iperf3 client of the same build measures it over the loopback of the host.
# Under the vm and sev the port is forwarded to the guest through the relay (SAGUARO_VM_INBOUND).
#
# The script does not probe the port first: iperf3 counts a connection that closes without a test as an
# error. The client is retried instead, until the server is up.
set -uo pipefail
: "${SAGUARO:=saguaro-direct}"
PORT=${IPERF_PORT:-5201}
SECONDS_=${IPERF_SECONDS:-2}
TRIES=${IPERF_TRIES:-60}

. ../common_tools/server-check.sh
server_start "$PORT:$PORT" "$SAGUARO" iperf3
trap 'server_stop; rm -f client.log server.log' EXIT

for i in $(seq "$TRIES"); do
    LD_LIBRARY_PATH=./install timeout 30 ./install/iperf3 -c 127.0.0.1 -p "$PORT" -t "$SECONDS_" \
        > client.log 2>&1 </dev/null
    grep -q "receiver" client.log && break
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: the server exited"; cat server.log; exit 1; }
    sleep 1
done
cat client.log
grep -q "receiver" client.log || { echo "FAIL: no receiver summary after $TRIES tries"; tail -n 20 server.log; exit 1; }
echo "[ Success 1/1 ]"
