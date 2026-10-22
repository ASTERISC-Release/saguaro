#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): redis-server runs under $GRAMINE and a TCP client on the host
# checks PING, SET, GET and 20 round trips (the checks of test/direct/test-direct.sh).
set -uo pipefail
: "${GRAMINE:=saguaro-direct}"
. ../common_tools/server-check.sh
server_start 6379:6379 "$GRAMINE" redis-server
python3 - <<'PYEOF'
import socket, time, sys
# Ready = a full PING round trip (the port is open before the server answers)
f = None
deadline = time.time() + 120
while f is None:
    try:
        s = socket.create_connection(("127.0.0.1", 6379), timeout=2)
        cand = s.makefile("rwb")
        cand.write(b"PING\r\n"); cand.flush()
        if cand.readline().strip() == b"+PONG":
            f = cand
        else:
            cand.close(); s.close(); time.sleep(0.25)
    except OSError:
        time.sleep(0.25)
    if f is None and time.time() > deadline:
        sys.exit("redis never answered PING (see server.log)")
def cmd(*lines):
    for l in lines: f.write((l + "\r\n").encode())
    f.flush(); return f.readline().strip()
assert cmd("PING") == b"+PONG", "PING"
assert cmd("SET k1 v1") == b"+OK", "SET"
assert cmd("GET k1") == b"$2", "GET header"
assert f.readline().strip() == b"v1", "GET value"
for i in range(20):
    val = f"val{i}"
    assert cmd(f"SET k{i} {val}") == b"+OK", i
    assert cmd(f"GET k{i}") == f"${len(val)}".encode(), i
    assert f.readline().strip() == val.encode(), i
print("redis: PING/SET/GET + 20 round trips OK")
PYEOF
rc=$?
server_stop
[ $rc = 0 ] && rm -f server.log
[ $rc = 0 ] && echo "[ Success 1/1 ]"
exit $rc
