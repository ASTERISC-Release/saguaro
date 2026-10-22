#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: an iperf3 server and an iperf3 client, each in its own Windows VBS enclave (the
# same image, started twice), talking over the loopback of the Windows host. SAGUARO is saguaro-vbs.
set -euo pipefail
: "${SAGUARO:=../../scripts/vbs/saguaro-vbs}"
PORT=${IPERF_PORT:-5201}
SECONDS_=${IPERF_SECONDS:-3}
STREAMS=${IPERF_STREAMS:-2}

"$SAGUARO" iperf3 -s -p "$PORT" -B 127.0.0.1 > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$SAGUARO" net wait "$PORT" 120

"$SAGUARO" iperf3 -c 127.0.0.1 -p "$PORT" -t "$SECONDS_" -P "$STREAMS" > client.log 2>&1
cat client.log
grep -q "receiver" client.log || { echo "FAIL: no receiver summary"; exit 1; }
echo "ok: iperf3 -c 127.0.0.1 -p $PORT -t $SECONDS_ -P $STREAMS (client in an enclave)"
echo "[ Success ]"
