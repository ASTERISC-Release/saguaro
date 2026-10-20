#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): the hyper server runs under $GRAMINE and curl on the host fetches
# its page from port 3000 ("Hello, World").
set -uo pipefail
: "${GRAMINE:=gramine-direct}"
. ../common_tools/server-check.sh
server_start 3000:3000 "$GRAMINE" rust-hyper-http-server
fetch() { curl -sf --max-time 5 http://127.0.0.1:3000/ -o fetched.html && grep -q "Hello, World" fetched.html; }
retry 120 fetch
rc=$?
rm -f fetched.html
server_stop
if [ $rc = 0 ]; then rm -f server.log; echo "rust: HTTP 200, Hello, World"; echo "[ Success 1/1 ]"; else tail -n 20 server.log; fi
exit $rc
