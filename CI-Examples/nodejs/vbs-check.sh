#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: Node.js in a Windows VBS enclave with --jitless (V8 interprets; no executable
# memory is created at run time) and a single V8 platform worker. helloworld.js, then heavy.js
# (JIT-hot loops, four worker threads, crypto and zlib, an HTTP server and client on 127.0.0.1).
# GRAMINE is gramine-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"
NODE_FLAGS="--jitless --v8-pool-size=1"

$GRAMINE nodejs $NODE_FLAGS /app/helloworld.js > OUTPUT
grep -q "Hello World" OUTPUT && echo "[ Success 1/2 ]"
rm -f OUTPUT

$GRAMINE nodejs $NODE_FLAGS /app/heavy.js
echo "[ Success 2/2 ]"
echo "[ Success ]"
