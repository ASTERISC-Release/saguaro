#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: Redis in a Windows VBS enclave, checked by a client on the Windows side
# (gramine-vbs net: the enclave's sockets are Windows sockets). GRAMINE is gramine-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"
PORT=${REDIS_PORT:-6379}

"$GRAMINE" redis-server > OUTPUT 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true' EXIT
"$GRAMINE" net wait "$PORT" 120

r() { "$GRAMINE" net redis "$PORT" "$@" | tr -d '\r'; }
expect() {  # expect "<wanted output>" redis-command...
    want=$1; shift
    got=$(r "$@")
    if [ "$got" != "$want" ]; then
        echo "FAIL: $* -> '$got' (wanted '$want')"; exit 1
    fi
    echo "ok: $*"
}
expect PONG PING
expect OK SET greeting "hello from a VBS enclave"
expect "hello from a VBS enclave" GET greeting
expect "(integer) 1" INCR counter
expect "(integer) 2" INCR counter
expect "(integer) 12" INCRBY counter 10
expect "(integer) 3" LPUSH list a b c
expect "$(printf '1) c\n2) b\n3) a')" LRANGE list 0 -1
expect "(integer) 1" DEL greeting
expect "(nil)" GET greeting
# many keys: 20000 SETs with 128-byte values over 5000 keys, then every key is there
"$GRAMINE" net bench redis-set "$PORT" -c 4 -n 20000 -k 5000 -d 128 -w 0
n=$(r DBSIZE | sed 's/(integer) //')
[ "$n" -ge 4900 ] || { echo "FAIL: DBSIZE $n after the SETs"; exit 1; }
echo "ok: DBSIZE $n"
"$GRAMINE" net bench redis-get "$PORT" -c 4 -n 20000 -k 5000 -w 0
# snapshots are off and must stay off (they would fork)
expect "$(printf '1) save\n2) ')" CONFIG GET save
echo "[ Success ]"
