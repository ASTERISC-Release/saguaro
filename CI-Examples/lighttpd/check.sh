#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): lighttpd runs under $GRAMINE and curl on the host fetches the
# 10 KiB test page, which must be byte-identical to the file.
set -uo pipefail
: "${GRAMINE:=saguaro-direct}"
. ../common_tools/server-check.sh
server_start 8003:8003 "$GRAMINE" ./lighttpd
retry 120 curl -sf --max-time 5 http://127.0.0.1:8003/random/10K.1.html -o fetched.html
rc=$?
if [ $rc = 0 ]; then
    cmp fetched.html install/html/random/10K.1.html; rc=$?
fi
rm -f fetched.html
server_stop
[ $rc = 0 ] && rm -f server.log
[ $rc = 0 ] && echo "lighttpd: 10240 bytes, identical to the file" && echo "[ Success 1/1 ]"
exit $rc
