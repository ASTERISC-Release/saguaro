#!/usr/bin/env bash

# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

set -e

if test -z "${GRAMINE:-}"; then
    if test -n "$SGX"; then
        GRAMINE=gramine-sgx
    else
        GRAMINE=gramine-direct
    fi
fi

# === helloworld ===
echo -e "\n\nRunning helloworld.py:"
$GRAMINE ./python scripts/helloworld.py > OUTPUT
grep -q "Hello World" OUTPUT && echo "[ Success 1/4 ]"
rm OUTPUT

# === web server and client (on port 8005) ===
echo -e "\n\nRunning HTTP server dummy-web-server.py in the background:"
GUEST_HTTP_PORT=${GRAMINE_VM_GUEST_HTTP_PORT:-8005}
HTTP_PORT=${GRAMINE_VM_HTTP_PORT:-$GUEST_HTTP_PORT}
stop_http_server() {
    test -f server.PID || return 0
    server_pid=$(cat server.PID)
    if test "${GRAMINE_VM:-0}" = 1; then
        kill -TERM -- "-$server_pid" 2>/dev/null || true
    else
        kill "$server_pid" 2>/dev/null || true
    fi
    wait "$server_pid" 2>/dev/null || true
    rm -f server.PID
}
if test "${GRAMINE_VM:-0}" = 1; then
    # Keep the VM launcher in its own process group so the guest and relay can be stopped together.
    setsid "$GRAMINE" ./python scripts/dummy-web-server.py "$GUEST_HTTP_PORT" &
else
    "$GRAMINE" ./python scripts/dummy-web-server.py "$GUEST_HTTP_PORT" &
fi
echo $! > server.PID
trap stop_http_server EXIT INT TERM
../../scripts/wait_for_server 300 127.0.0.1 "$HTTP_PORT"

echo -e "\n\nRunning HTTP client test-http.py:"
if test "${GRAMINE_VM:-0}" = 1; then
    # Outbound guest connections and guest loopback are separate later phases. For phase 2, run
    # this example's HTTP client in the container and reach the guest server through the relay.
    python3 scripts/test-http.py 127.0.0.1 "$HTTP_PORT" > OUTPUT1
else
    "$GRAMINE" ./python scripts/test-http.py 127.0.0.1 "$GUEST_HTTP_PORT" > OUTPUT1
fi
wget -q "http://127.0.0.1:$HTTP_PORT/" -O OUTPUT2
echo >> OUTPUT2  # include newline since wget doesn't add it
# check if all lines from OUTPUT2 are included in OUTPUT1
# TODO: simplify after fixing Gramine logging subsystem, which currently mixes its output with the
# application output.
diff OUTPUT1 OUTPUT2 | grep -q '^>' || echo "[ Success 2/4 ]"
stop_http_server
trap - EXIT INT TERM
rm -f OUTPUT1 OUTPUT2

# === numpy ===
$GRAMINE ./python scripts/test-numpy.py > OUTPUT
grep -q "dot: " OUTPUT && echo "[ Success 3/4 ]"
rm OUTPUT

# === scipy ===
$GRAMINE ./python scripts/test-scipy.py > OUTPUT
grep -q "cholesky: " OUTPUT && echo "[ Success 4/4 ]"
rm OUTPUT

# === SGX quote ===
if test -n "$SGX"
then
    $GRAMINE ./python scripts/sgx-report.py > OUTPUT
    grep -q "Generated SGX report" OUTPUT && echo "[ Success SGX report ]"
    rm OUTPUT

    $GRAMINE ./python scripts/sgx-quote.py > OUTPUT
    grep -q "Extracted SGX quote" OUTPUT && echo "[ Success SGX quote ]"
    rm OUTPUT
fi
