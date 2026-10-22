#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check`: the TLS echo server in a VBS enclave and the Windows client that verifies its
# attestation before it sends anything (saguaro-vbs tls-client). The positive case, then the
# negative ones: each must be refused (exit 1, "REFUSED").
set -uo pipefail
: "${GRAMINE:=../../../scripts/vbs/saguaro-vbs}"
PORT=${TLS_PORT:-4433}
FAMILY=7361677561726f2d7662732d746c7300
IMAGE=746c732d6563686f0000000000000001
fails=0
pins=(--verifier tls-echo.vbs --family-id $FAMILY --image-id $IMAGE --min-svn 1)

start() {  # image args...
    local app=$1; shift
    # (with loader.insecure__use_cmdline_argv the host must pass at least one argument)
    "$GRAMINE" "$app" --port "$PORT" "$@" > "OUTPUT-$app" 2>&1 &
    pid=$!
    "$GRAMINE" net wait "$PORT" 120 > /dev/null || { echo "FAIL: server did not start"; exit 1; }
}
stop() { kill $pid 2>/dev/null; wait $pid 2>/dev/null; sleep 1; }
trap 'kill $pid 2>/dev/null || true' EXIT
expect() {  # expect accept|refuse "label" client-args...
    local want=$1 label=$2; shift 2
    out=$("$GRAMINE" tls-client "$PORT" "$@" 2>&1 | tr -d '\r')
    rc=$?
    if { [ "$want" = accept ] && [ $rc -eq 0 ] && grep -q '^echo: ' <<<"$out"; } ||
       { [ "$want" = refuse ] && [ $rc -ne 0 ] && grep -q '^REFUSED' <<<"$out" && ! grep -q '^echo: ' <<<"$out"; }; then
        echo "ok: $label: $(grep -E '^(REFUSED|attestation|echo)' <<<"$out" | head -2 | tr '\n' ' ')"
    else
        echo "FAIL: $label (exit $rc)"; echo "$out"; fails=$((fails + 1))
    fi
}

start tls-echo
expect accept "genuine server, all pins" "${pins[@]}" --message "hello through TLS"
expect refuse "wrong image id pinned" --verifier tls-echo.vbs --family-id $FAMILY \
    --image-id 746c732d6563686f0000000000000002 --min-svn 1
expect refuse "wrong family id pinned" --verifier tls-echo.vbs --family-id 7361677561726f2d7662732d746c7301 --image-id $IMAGE
expect refuse "SVN below the minimum" --verifier tls-echo.vbs --min-svn 2
stop

start tls-echo --test-wrong-key
expect refuse "report bound to another key" "${pins[@]}"
stop
start tls-echo --test-tamper-report
expect refuse "tampered report (user data rewritten)" "${pins[@]}"
stop

if [ -d tls-echo-debug.vbs ]; then
    start tls-echo-debug
    expect refuse "debug enclave" --verifier tls-echo-debug.vbs --family-id $FAMILY --image-id $IMAGE
    expect accept "debug enclave with --allow-debug" --verifier tls-echo-debug.vbs \
        --family-id $FAMILY --image-id $IMAGE --allow-debug
    expect refuse "debug enclave verified as the release image" "${pins[@]}" --allow-debug
    stop
else
    echo "skipped: debug enclave (build it with make VBS_DEBUG=1)"
fi

[ $fails -eq 0 ] && echo "[ Success ]" || { echo "[ $fails FAILED ]"; exit 1; }
