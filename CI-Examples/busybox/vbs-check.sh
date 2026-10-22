#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: single BusyBox applets, one enclave each: echo, cat and ls of a packed file,
# grep, a shell with builtins only, and wget over plain HTTP from a server that runs natively in WSL
# on 127.0.0.1 (Windows forwards its loopback to WSL). GRAMINE is saguaro-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/saguaro-vbs}"
PORT=${BUSYBOX_PORT:-19112}

expect() {  # expect "<wanted output>" applet args...
    want=$1; shift
    got=$("$GRAMINE" busybox "$@" | tr -d '\r')
    if [ "$got" != "$want" ]; then
        echo "FAIL: busybox $* -> '$got' (wanted '$want')"; exit 1
    fi
    echo "ok: busybox $*"
}

expect "Hello" echo Hello
expect "$(cat helper-files/hosts)" cat /etc/hosts
expect "3" sh -c 'x=1; echo $((x+2))'
expect "$(grep -c localhost helper-files/hosts)" grep -c localhost /etc/hosts
"$GRAMINE" busybox ls /etc | tr -d '\r' | grep -qx hosts && echo "ok: busybox ls /etc (hosts listed)"

(cd helper-files; exec python3 -m http.server -b 127.0.0.1 "$PORT") > httpd.log 2>&1 &
httpd_pid=$!
trap 'kill $httpd_pid 2>/dev/null || true; wait $httpd_pid 2>/dev/null || true' EXIT
../common_tools/wait_for_server 5 127.0.0.1 "$PORT"
# the enclave has no working directory of its own (its paths are below the host root): the page goes
# to stdout
expect "$(cat helper-files/hosts)" wget -q -O - "http://127.0.0.1:$PORT/hosts"
echo "[ Success ]"
