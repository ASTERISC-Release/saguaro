#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: memcached in a Windows VBS enclave, checked by a client on the Windows side
# (saguaro-vbs net: the enclave's sockets are Windows sockets). SAGUARO is saguaro-vbs.
set -euo pipefail
: "${SAGUARO:=../../scripts/vbs/saguaro-vbs}"
PORT=${MEMCACHED_PORT:-11211}

"$SAGUARO" memcached > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$SAGUARO" net wait "$PORT" 120

req() { "$SAGUARO" net req "$PORT" "$1" 3000 | tr -d '\r'; }
expect() {  # expect "<wanted reply>" "<request with \r\n escapes>"
    got=$(req "$2")
    if [ "$got" != "$1" ]; then
        echo "FAIL: $2 -> '$got' (wanted '$1')"; exit 1
    fi
    echo "ok: $2"
}
expect "$(printf 'STORED\nVALUE greeting 5 25\nhello from a VBS enclave!\nEND')" \
    'set greeting 5 0 25\r\nhello from a VBS enclave!\r\nget greeting\r\nquit\r\n'
expect "$(printf 'STORED\n11\n12\nDELETED\nEND')" \
    'set n 0 0 2\r\n10\r\nincr n 1\r\nincr n 1\r\ndelete n\r\nget n\r\nquit\r\n'
"$SAGUARO" net req "$PORT" 'version\r\nquit\r\n' 3000 | tr -d '\r'
"$SAGUARO" net bench memcached-set "$PORT" -c 4 -n 20000 -k 5000 -d 128 -w 0
"$SAGUARO" net bench memcached-get "$PORT" -c 4 -n 20000 -k 5000 -w 0
items=$("$SAGUARO" net req "$PORT" 'stats\r\nquit\r\n' 3000 | tr -d '\r' | awk '$2 == "curr_items" { print $3 }')
[ "${items:-0}" -ge 4900 ] || { echo "FAIL: curr_items ${items:-?} after the sets"; exit 1; }
echo "ok: curr_items $items"
echo "[ Success ]"
