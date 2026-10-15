#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: the hyper server runs in a Windows VBS enclave on 127.0.0.1:3000 and is
# checked by a request from the Windows side (gramine-vbs net), then a short benchmark.
# GRAMINE is gramine-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"
PORT=3000

"$GRAMINE" rust-hyper-http-server > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$GRAMINE" net wait "$PORT" 120

got=$("$GRAMINE" net req "$PORT" 'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | tr -d '\r')
echo "$got" | grep -q "Hello, World" || { echo "FAIL: response: $got"; exit 1; }
echo "ok: GET / -> Hello, World"
"$GRAMINE" net bench http "$PORT" -c 4 -n 2000 -p / -w 0
echo "[ Success ]"
