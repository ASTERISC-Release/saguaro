#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: curl in a Windows VBS enclave fetches a page from a plain HTTP server that runs
# natively in WSL on 127.0.0.1 (Windows forwards its loopback to WSL). SAGUARO is saguaro-vbs.
set -euo pipefail
: "${SAGUARO:=../../scripts/vbs/saguaro-vbs}"
PORT=${CURL_PORT:-19111}

(cd test-docroot; exec python3 -m http.server -b 127.0.0.1 "$PORT") > httpd.log 2>&1 &
httpd_pid=$!
trap 'kill $httpd_pid 2>/dev/null || true; wait $httpd_pid 2>/dev/null || true' EXIT
../common_tools/wait_for_server 5 127.0.0.1 "$PORT"

"$SAGUARO" curl -s "http://127.0.0.1:$PORT/" > OUTPUT
cat OUTPUT
grep -q "Hello World" OUTPUT || { echo "FAIL: no 'Hello World' in the page"; exit 1; }
echo "[ Success 1/1 ]"
echo "[ Success ]"
