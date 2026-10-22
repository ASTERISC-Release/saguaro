#!/usr/bin/env bash

# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

set -e

# VBS=1: SAGUARO is scripts/vbs/saguaro-vbs (make VBS=1 check); the servers listen on Windows, so
# the port is awaited and fetched on the Windows side (saguaro-vbs net / win)
if test -z "${SAGUARO:-}"; then
    if test -n "$SGX"; then
        SAGUARO=saguaro-sgx
    else
        SAGUARO=saguaro-direct
    fi
fi

# === helloworld ===
echo -e "\n\nRunning helloworld.py:"
$SAGUARO ./python scripts/helloworld.py > OUTPUT
grep -q "Hello World" OUTPUT && echo "[ Success 1/4 ]"
rm OUTPUT

# === web server and client (on port 8005) ===
echo -e "\n\nRunning HTTP server dummy-web-server.py in the background:"
GUEST_HTTP_PORT=${SAGUARO_VM_GUEST_HTTP_PORT:-8005}
HTTP_PORT=${SAGUARO_VM_HTTP_PORT:-$GUEST_HTTP_PORT}
stop_http_server() {
    test -f server.PID || return 0
    server_pid=$(cat server.PID)
    if test "${SAGUARO_VM:-0}" = 1; then
        kill -TERM -- "-$server_pid" 2>/dev/null || true
    else
        kill "$server_pid" 2>/dev/null || true
    fi
    wait "$server_pid" 2>/dev/null || true
    rm -f server.PID
}
if test "${SAGUARO_VM:-0}" = 1; then
    # Keep the VM launcher in its own process group so the guest and relay can be stopped together.
    setsid "$SAGUARO" ./python scripts/dummy-web-server.py "$GUEST_HTTP_PORT" &
else
    "$SAGUARO" ./python scripts/dummy-web-server.py "$GUEST_HTTP_PORT" &
fi
echo $! > server.PID
trap stop_http_server EXIT INT TERM
if test -n "${VBS:-}"; then
    "$SAGUARO" net wait "$HTTP_PORT" 300
else
    ../../scripts/wait_for_server 300 127.0.0.1 "$HTTP_PORT"
fi

echo -e "\n\nRunning HTTP client test-http.py:"
if test "${SAGUARO_VM:-0}" = 1; then
    # Outbound guest connections and guest loopback are separate later phases. For phase 2, run
    # this example's HTTP client in the container and reach the guest server through the relay.
    # The relay listens on the host before the guest is up, so the wait above ends at once; a guest
    # that boots slowly (SEV and SEV-SNP take 4 to 8 s) is not reachable yet. Try again until it is.
    http_ok=0
    for _ in $(seq 120); do
        if python3 scripts/test-http.py 127.0.0.1 "$HTTP_PORT" > OUTPUT1 2>/dev/null; then
            http_ok=1
            break
        fi
        sleep 0.5
    done
    test "$http_ok" = 1 || python3 scripts/test-http.py 127.0.0.1 "$HTTP_PORT" > OUTPUT1
else
    "$SAGUARO" ./python scripts/test-http.py 127.0.0.1 "$GUEST_HTTP_PORT" > OUTPUT1
fi
if test -n "${VBS:-}"; then
    "$SAGUARO" win curl.exe -s "http://127.0.0.1:$HTTP_PORT/" > OUTPUT2
else
    wget -q "http://127.0.0.1:$HTTP_PORT/" -O OUTPUT2
fi
echo >> OUTPUT2  # include newline since wget doesn't add it
# check if all lines from OUTPUT2 are included in OUTPUT1
# TODO: simplify after fixing Saguaro logging subsystem, which currently mixes its output with the
# application output.
diff OUTPUT1 OUTPUT2 | grep -q '^>' || echo "[ Success 2/4 ]"
stop_http_server
trap - EXIT INT TERM
rm -f OUTPUT1 OUTPUT2

# === numpy ===
# (not packed in the aarch64 VBS example: Makefile)
if test -z "${VBS_NO_NUMPY:-}"; then
$SAGUARO ./python scripts/test-numpy.py > OUTPUT
grep -q "dot: " OUTPUT && echo "[ Success 3/4 ]"
rm OUTPUT

# === scipy ===
$SAGUARO ./python scripts/test-scipy.py > OUTPUT
grep -q "cholesky: " OUTPUT && echo "[ Success 4/4 ]"
rm OUTPUT
fi

# === SGX quote ===
if test -n "$SGX"
then
    $SAGUARO ./python scripts/sgx-report.py > OUTPUT
    grep -q "Generated SGX report" OUTPUT && echo "[ Success SGX report ]"
    rm OUTPUT

    # the quote needs remote attestation (RA_TYPE=dcap, the AESM service and a PCCS)
    if test "${RA_TYPE:-none}" != none
    then
        $SAGUARO ./python scripts/sgx-quote.py > OUTPUT
        grep -q "Extracted SGX quote" OUTPUT && echo "[ Success SGX quote ]"
        rm OUTPUT
    fi
fi
