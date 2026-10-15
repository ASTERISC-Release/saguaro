#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: iperf3 in Windows VBS enclaves, a server and a client, both in enclaves, over
# the Windows loopback (GRAMINE is gramine-vbs). The server listens on 127.0.0.1:$IPERF_PORT; the
# client runs two streams for a few seconds and must report a receiver summary and exit 0.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"
PORT=${IPERF_PORT:-5201}

"$GRAMINE" iperf3 -s -p "$PORT" -B 127.0.0.1 > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$GRAMINE" net wait "$PORT" 120

"$GRAMINE" iperf3 -c 127.0.0.1 -p "$PORT" -t 3 -P 2 > client-OUTPUT 2>&1
cat client-OUTPUT
grep -q "receiver" client-OUTPUT || { echo "FAIL: no receiver summary"; exit 1; }
grep -q "iperf Done." client-OUTPUT || { echo "FAIL: client did not finish"; exit 1; }
echo "[ Success ]"
