#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# `make VBS=1 check`: the TCP server in a VBS enclave answers clients on the Windows side
# (gramine-vbs net req: the enclave's sockets are Windows sockets).
set -euo pipefail
: "${GRAMINE:=../../../scripts/vbs/gramine-vbs}"
PORT=18080
"$GRAMINE" tcpserver > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$GRAMINE" net wait "$PORT" 120
# (net wait connects once and counts as the first client of the server, which serves three)
for i in 1 2; do
    got=$("$GRAMINE" net req "$PORT" "hello $i" | tr -d '\r')
    case "$got" in *"echo from the enclave: hello $i"*) echo "ok: client $i" ;;
        *) echo "FAIL: client $i got '$got'"; exit 1 ;; esac
done
wait $pid
grep -q "served 3 clients" OUTPUT || { echo "FAIL: server output"; cat OUTPUT; exit 1; }
echo "[ Success ]"
