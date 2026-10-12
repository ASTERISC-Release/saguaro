# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# The per-VM vsock relay of the VM PAL, for the launchers (Documentation/devel/vm-network/PLAN.md).
# This file is sourced by `gramine-vm-aarch64` (and, once the x86_64 launcher is switched over, by
# `gramine-vm`); it only defines functions. It is installed next to the VM PAL, as
# $HOST_PAL_PATH/vm-relay.sh.
#
# One relay process runs per VM, in the launcher's container or host, started once the guest CID is
# known and stopped with the VM. It listens on AF_VSOCK port 65536 + CID and handles the guest's
# outbound destination handshakes, UDP/DNS, loopback hairpinning, and the static inbound forwards of the
# manifest's `vm.net.inbound` (see python/graminelibos/vsock_relay.py).
#
#   vm_relay_find_python <launcher>
#       make `graminelibos` importable: when it is not, add <prefix>/lib/python3*/site-packages of the
#       installation that holds <launcher> (<prefix>/bin/gramine-vm) to PYTHONPATH. Callers with a
#       stripped environment (the LibOS regression tests pass `env=`) have no PYTHONPATH.
#   vm_relay_manifest_wants_network <manifest>
#       exit status 0 when the manifest has a `vm.net` table (it needs the vsock device and the relay).
#   vm_relay_start <cid> <manifest>
#       start the relay; sets RELAY_PID and RELAY_LOG. Returns 0 (also when the relay is disabled with
#       GRAMINE_VM_RELAY=0), or 1 after printing why: the caller exits with status 2.
#   vm_relay_stop
#       stop the relay and print what it logged after startup; safe to call more than once. Meant for
#       the launcher's EXIT trap.
#
# Environment:
#   GRAMINE_VM_RELAY=0         do not start a relay
#   GRAMINE_VM_INBOUND=g:h,... more inbound rules, "guest[:host]" each, for drivers that cannot edit the
#                              manifest
#   GRAMINE_VM_RELAY_DEBUG=1   relay debug trace and asyncio debug mode

RELAY_PID=
RELAY_LOG=

vm_relay_find_python() {
    local launcher=$1 prefix site
    if python3 -c 'import graminelibos' 2>/dev/null; then
        return 0
    fi
    prefix=$(cd "$(dirname "$(readlink -f "$launcher")")/.." && pwd)
    for site in "$prefix"/lib/python3*/site-packages; do
        if [ -d "$site/graminelibos" ]; then
            export PYTHONPATH="$site${PYTHONPATH:+:$PYTHONPATH}"
            return 0
        fi
    done
    return 1
}

vm_relay_manifest_wants_network() {
    python3 - "$1" <<'EOF' 2>/dev/null
import sys
try:
    import tomllib
except ImportError:
    import tomli as tomllib
with open(sys.argv[1], 'rb') as f:
    manifest = tomllib.load(f)
sys.exit(0 if manifest.get('vm', {}).get('net') else 1)
EOF
}

vm_relay_start() {
    local cid=$1 manifest=$2 rule
    local -a rules args=()
    RELAY_PID=
    RELAY_LOG=
    [ "${GRAMINE_VM_RELAY:-1}" = "0" ] && return 0

    if ! python3 -c 'import graminelibos.vsock_relay' 2>/dev/null; then
        echo "Error: the VM's vsock relay needs the Python package graminelibos of this Gramine" \
             "installation (set PYTHONPATH to <prefix>/lib/python3*/site-packages)" >&2
        return 1
    fi

    RELAY_LOG=$(mktemp /tmp/gramine-vm-relay.XXXXXX)
    IFS=, read -ra rules <<< "${GRAMINE_VM_INBOUND:-}"
    for rule in "${rules[@]}"; do
        [ -n "$rule" ] && args+=(--inbound "$rule")
    done
    # PYTHONASYNCIODEBUG makes the relay log event-loop stalls ("Executing ... took 0.sss seconds"),
    # which is how a blocking vsock dial that freezes the other forwards shows up.
    env PYTHONASYNCIODEBUG=${GRAMINE_VM_RELAY_DEBUG:+1} \
        python3 -m graminelibos.vsock_relay --cid "$cid" --config "$manifest" --die-with-parent \
        "${args[@]}" \
        >"$RELAY_LOG" 2>&1 &
    RELAY_PID=$!
    # wait until the relay prints that it listens (or dies: a host port that is taken fails here,
    # not silently at the first connection)
    for _ in $(seq 100); do
        grep -q "listening on vsock port" "$RELAY_LOG" 2>/dev/null && break
        kill -0 "$RELAY_PID" 2>/dev/null || break
        sleep 0.05
    done
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        echo "Error: the VM's vsock relay did not start (host ports of vm.net.inbound must be" \
             "free):" >&2
        cat "$RELAY_LOG" >&2
        rm -f "$RELAY_LOG"
        RELAY_PID=
        RELAY_LOG=
        return 1
    fi
    grep "^vm-relay:" "$RELAY_LOG" >&2
    return 0
}

vm_relay_stop() {
    # the relay is a single asyncio process (no children to reap); stop it and surface any line it
    # wrote after startup (a forward that could not reach the guest, or its debug trace) before
    # deleting the log -- the startup lines were already echoed by vm_relay_start
    if [ -n "${RELAY_PID:-}" ]; then
        kill "$RELAY_PID" 2>/dev/null
        wait "$RELAY_PID" 2>/dev/null
        RELAY_PID=
    fi
    if [ -n "${RELAY_LOG:-}" ] && [ -s "$RELAY_LOG" ]; then
        grep -vE '^vm-relay: (cid=[0-9]+ (port=[0-9]+ ->|listening))' "$RELAY_LOG" >&2
    fi
    if [ -n "${RELAY_LOG:-}" ]; then
        rm -f "$RELAY_LOG"
        RELAY_LOG=
    fi
}
