#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: lighttpd in a Windows VBS enclave, checked from the Windows side (the enclave's
# sockets are Windows sockets): pages fetched with curl.exe and compared with the packed files.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"
PORT=${PORT:-8003}
ROOT=vbs-build/root/lighttpd/html

"$GRAMINE" lighttpd > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$GRAMINE" net wait "$PORT" 120

fetch() {  # path -> the body on stdout
    "$GRAMINE" win curl.exe -s -f "http://127.0.0.1:$PORT$1"
}
for f in random/100.1.html random/2K.1.html random/10K.1.html random/100K.1.html \
         random/1M.1.html; do
    path=/$f
    if fetch "$path" | cmp -s - "$ROOT/$f"; then
        echo "ok: GET $path ($(stat -c %s "$ROOT/$f") bytes, identical)"
    else
        echo "FAIL: GET $path"; exit 1
    fi
done
code=$("$GRAMINE" win curl.exe -s -o NUL -w "%{http_code}" "http://127.0.0.1:$PORT/no-such-file")
[ "$code" = 404 ] || { echo "FAIL: a missing file gave $code"; exit 1; }
echo "ok: GET /no-such-file -> 404"
# keep-alive under load: 2000 requests over 4 connections
"$GRAMINE" net bench http "$PORT" -c 4 -n 2000 -p /random/10K.1.html -w 0
echo "[ Success ]"
