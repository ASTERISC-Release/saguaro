#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: helloworld.js and heavy.js with --jitless (no JIT memory) and one V8 platform
# thread, in a Windows VBS enclave. GRAMINE is gramine-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"
ARGS=(--jitless --v8-pool-size=1)

out=$("$GRAMINE" node "${ARGS[@]}" /app/helloworld.js | tr -d '\r')
echo "$out"
[ "$out" = "Hello World" ] || { echo "FAIL: helloworld.js"; exit 1; }
echo "[ Success 1/2 ]"

out=$("$GRAMINE" node "${ARGS[@]}" /app/heavy.js | tr -d '\r')
echo "$out"
echo "$out" | grep -q "^HEAVY OK" || { echo "FAIL: heavy.js"; exit 1; }
echo "[ Success 2/2 ]"
echo "[ Success ]"
