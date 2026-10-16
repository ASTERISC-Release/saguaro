#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: the hyper server runs in a Windows VBS enclave on 127.0.0.1:3000 (hard-coded in
# src/main.rs) and Windows' own curl.exe asks it for a page. GRAMINE is gramine-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"

"$GRAMINE" rust-hyper-http-server > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$GRAMINE" net wait 3000 120

"$GRAMINE" win curl.exe -s http://127.0.0.1:3000/ > page.txt
cat page.txt; echo
grep -q "Hello, World" page.txt || { echo "FAIL: no 'Hello, World' in the response"; exit 1; }
echo "[ Success ]"
