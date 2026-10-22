#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): memcached runs under $GRAMINE and a TCP client on the host
# checks counters and 100 set/get round trips (the checks of test/direct/test-direct.sh).
set -uo pipefail
: "${GRAMINE:=saguaro-direct}"
. ../common_tools/server-check.sh
server_start 11211:11211 "$GRAMINE" memcached
python3 - <<'PYEOF'
import socket, time, sys
# Ready = a full "version" round trip (the port is open before the server answers)
f = None
deadline = time.time() + 120
while f is None:
    try:
        s = socket.create_connection(("127.0.0.1", 11211), timeout=2)
        cand = s.makefile("rwb")
        cand.write(b"version\r\n"); cand.flush()
        if cand.readline().strip().startswith(b"VERSION"):
            f = cand
        else:
            cand.close(); s.close(); time.sleep(0.25)
    except OSError:
        time.sleep(0.25)
    if f is None and time.time() > deadline:
        sys.exit("memcached never answered 'version' (see server.log)")
def cmd(*lines):
    for l in lines: f.write((l + "\r\n").encode())
    f.flush(); return f.readline().strip()
# the first argument is the command line, a second one the value line (its length must be exact)
assert cmd("set gcounter 0 60 1", "0") == b"STORED"
assert cmd("incr gcounter 1") == b"1"
assert cmd("incr gcounter 1") == b"2"
assert cmd("get gcounter") == b"VALUE gcounter 0 1"
assert f.readline().strip() == b"2" and f.readline().strip() == b"END"
for i in range(100):
    val = f"v{i:04d}"
    assert cmd(f"set k{i} 0 60 5", val) == b"STORED", i
    assert cmd(f"get k{i}") == b"VALUE k%d 0 5" % i, i
    assert f.readline().strip().decode() == val, i
    assert f.readline().strip() == b"END", i
assert cmd("delete k5") == b"DELETED"
assert cmd("get k5") == b"END"
print("memcached: counter semantics + 100 set/get round trips OK")
PYEOF
rc=$?
server_stop
[ $rc = 0 ] && rm -f server.log
[ $rc = 0 ] && echo "[ Success 1/1 ]"
exit $rc
