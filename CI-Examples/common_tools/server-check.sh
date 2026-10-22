# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Helpers for the `check.sh` of the server examples (memcached, redis, lighttpd, nginx). Source this
# file from a check.sh, which runs in the example's directory:
#
#   server_start <inbound> <command...>   start the server in the background, its output in server.log;
#                                         <inbound> is SAGUARO_VM_INBOUND, the ports the vm and sev
#                                         relay forwards to the guest (ignored by direct)
#   server_stop                           stop it, with everything it started (also on exit)
#   retry <seconds> <command...>          run the command until it succeeds or the time is up
#
# The server runs in its own session under `timeout`, so server_stop kills the whole group: killing only
# gramine-vm would leave its QEMU process running (and exec.sh waiting for it).

SERVER_PID=

server_start() {
    local inbound=$1; shift
    SAGUARO_VM_INBOUND="$inbound" setsid timeout -s KILL "${SERVER_TIMEOUT:-300}" "$@" \
        >server.log 2>&1 </dev/null &
    SERVER_PID=$!
    trap server_stop EXIT
}

server_stop() {
    [ -n "$SERVER_PID" ] || return 0
    kill -KILL -- "-$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=
}

retry() {
    local seconds=$1 i; shift
    for i in $(seq 1 $((seconds * 2))); do
        "$@" && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || { echo "the server exited"; tail -n 20 server.log; return 1; }
        sleep 0.5
    done
    return 1
}
