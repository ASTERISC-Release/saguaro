#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: BusyBox applets in a Windows VBS enclave, one enclave run per applet (echo, cat,
# ls, grep, and the shell's arithmetic). GRAMINE is gramine-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"

check() {  # check "<wanted>" applet args...
    want=$1; shift
    got=$("$GRAMINE" busybox "$@" | tr -d '\r')
    if [ "$got" != "$want" ]; then
        echo "FAIL: busybox $* -> '$got' (wanted '$want')"; exit 1
    fi
    echo "ok: busybox $*"
}
check "Hello" echo Hello
check "$(printf 'Hello from a file packed into a VBS enclave\nsecond line')" cat /app/data.txt
check "2" grep -c line /app/data.txt
check "3" sh -c 'x=1; echo $((x+2))'
check "/app/data.txt" ls /app/data.txt
echo "[ Success ]"
