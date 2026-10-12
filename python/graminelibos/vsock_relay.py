# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""
vsock_relay -- the per-VM network relay of the VM PAL (Documentation/devel/vm-network/PLAN.md).

One relay process per VM, started by `gramine-vm` in the container and stopped with the VM. It
listens on AF_VSOCK port 65536 + <CID> (RELAY_PORT_BASE + CID; the guest knows its own CID, so the
relay address needs no configuration) and, per the plan's `vm.net.inbound`, forwards TCP
connections:

    host (container) TCP  bind:host   ->   guest CID:guest

For each inbound rule the relay listens on the host address `bind:host`; every accepted connection
is bridged to a fresh AF_VSOCK connection to (CID, `guest`) -- the vsock socket the guest's emulated
TCP listener for that port appears as (see pal/src/host/vm-common/pal_common_sockets.c, which keeps
only the port of a bound AF_INET address). Bytes are pumped both ways until either side reaches EOF.
This is exactly the connection the phase-1 regression client opens (test/vsock-loopback/), so a VM
application that binds a TCP port needs no changes to be reachable through the relay.

Relay v0 does static inbound forwarding only. The vsock listener on the relay port is opened from
v0 (it reserves 65536+CID and lets `gramine-vm` check that the relay came up before booting the
guest), but v0 has no guest-initiated protocol: a connection opened from the VM to the relay is
closed with a short goodbye. The destination handshake that will carry outbound traffic is phase 3
of the plan; UDP/DNS and loopback are later phases.

AF_VSOCK sockets are created explicitly and handed to asyncio via create_server(sock=...) /
create_connection(sock=...): the `family=`/`host=` fast paths of start_server/open_connection go
through getaddrinfo, whose AF_VSOCK handling varies between Python versions.

Usage: python3 -m graminelibos.vsock_relay --cid CID [--inbound RULE]... [--config FILE]
  --inbound  "guest=G[:host=H[:bind=B]]" or the short forms "G" / "G:H"
  --config   a TOML file whose `inbound` key holds the rule list (the shape `vm.net.inbound` has)

A host port that cannot be bound is a fatal error (an explicit choice in the plan: no silent
clash). One "vm-relay: ..." line is printed per forward and one when the vsock listener is up; a
configuration error exits 2, a bind error exits 3.
"""

import argparse
import asyncio
import os
import socket
import sys
import time

try:
    import tomllib
except ImportError:
    import tomli as tomllib

# The guest reaches its relay at this vsock port. 65536 keeps the relay clear of the ports a VM
# application binds or reaches directly (the graphics direct mapping, later
# vm.net.direct_vsock_ports); deriving the port from the CID keeps the VMs of one host apart in the
# host's global vsock port space.
RELAY_PORT_BASE = 65536

# A TCP listener default backlog; the OS queues connections to a slow guest like it does for any
# server, so this only has to be generous.
LISTEN_BACKLOG = 128

# The most bytes one forwarded connection may hold in memory while the other side is slower (or has
# stopped reading); beyond it the faster direction is paused, which is the flow control asyncio's
# write buffer gives us. Not a security limit -- relay v0 is loopback-bound by default.
READ_CHUNK = 64 * 1024


def relay_port(cid):
    return RELAY_PORT_BASE + cid


class Rule:
    """One inbound forward: host `bind:host` -> guest `guest`."""

    def __init__(self, guest, host=None, bind='127.0.0.1'):
        self.guest = int(guest)
        self.host = int(host if host is not None else guest)
        self.bind = str(bind)
        for name, port in (('guest', self.guest), ('host', self.host)):
            if not 0 < port < 65536:
                raise ValueError(f'inbound rule {name} port out of range: {port}')


def parse_rule(spec):
    """`guest=8080:host=18080:bind=127.0.0.1`, or the short form `8080:18080`, or `8080`."""
    if '=' in spec:
        fields = {}
        for part in spec.split(':'):
            if not part:
                continue
            key, sep, value = part.partition('=')
            if not sep or not value:
                raise ValueError(f'bad inbound rule {spec!r}: {part!r} is not key=value')
            fields[key] = value
        unknown = set(fields) - {'guest', 'host', 'bind'}
        if unknown:
            raise ValueError(f'bad inbound rule {spec!r}: unknown key(s) {sorted(unknown)}')
        if 'guest' not in fields:
            raise ValueError(f'bad inbound rule {spec!r}: no guest port')
        return Rule(fields['guest'], fields.get('host'), fields.get('bind', '127.0.0.1'))
    parts = spec.split(':')
    if not 1 <= len(parts) <= 2 or not all(p.isdigit() for p in parts):
        raise ValueError(f'bad inbound rule {spec!r}: expected G, G:H, or guest=...')
    return Rule(parts[0], parts[1] if len(parts) == 2 else None)


def rules_from_config(text):
    """The `inbound` list of a TOML file, in the shape `vm.net.inbound` will have."""
    data = tomllib.loads(text)
    inbound = data.get('inbound') or []
    rules = []
    for item in inbound:
        if isinstance(item, dict):
            if 'guest' not in item:
                raise ValueError(f'bad inbound rule {item!r}: no guest port')
            rules.append(Rule(item['guest'], item.get('host'), item.get('bind', '127.0.0.1')))
        else:
            rules.append(parse_rule(str(item)))
    return rules


def load_rules(args):
    """--inbound rules plus --config rules, checked for host-address clashes."""
    rules = [parse_rule(spec) for spec in args.inbound]
    if args.config:
        with open(args.config, 'rb') as f:
            rules += rules_from_config(f.read().decode('utf-8'))
    seen = {}
    for rule in rules:
        key = (rule.bind, rule.host)
        if key in seen:
            raise ValueError(f'two inbound rules map to the same host address'
                             f' {rule.bind}:{rule.host}')
        seen[key] = rule
    return rules


class Stats:
    """Counters for one relay process (single-threaded asyncio, so plain attributes are fine)."""

    def __init__(self):
        self.accepted = 0          # host connections accepted for a forward
        self.guest_connections = 0 # vsock connections opened by the VM (v0: refused)
        self.forward_error = 0     # forwards that could not reach the guest or died


def new_vsock_listener(cid, port):
    """A bound, listening AF_VSOCK socket for (VMADDR_CID_ANY, port)."""
    sock = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((socket.VMADDR_CID_ANY, port))
    sock.listen(LISTEN_BACKLOG)
    sock.setblocking(False)
    return sock


def vsock_connect(cid, guest_port):
    """
    A connected, non-blocking AF_VSOCK socket to (cid, guest_port), or an OSError (connection
    refused if the guest has not bound that port, host unreachable while the guest is booting, ...).
    A blocking connect with a short timeout keeps a bad guest port from hanging the relay: the
    guest answers within milliseconds once its listener is up. The caller wraps it in a stream.
    """
    sock = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    sock.setblocking(True)
    sock.settimeout(10)
    try:
        sock.connect((cid, guest_port))
    except OSError:
        sock.close()
        raise
    sock.setblocking(False)
    return sock


DEBUG = bool(os.environ.get('GRAMINE_VM_RELAY_DEBUG'))
# a monotonic clock origin for the debug trace (the relay's own startup; no wall time needed)
_T0 = time.monotonic()


def log_debug(msg):
    if DEBUG:
        print(f'vm-relay dbg {time.monotonic() - _T0:9.3f} {msg}',
              file=sys.stderr, flush=True)


async def pump(name, reader, writer):
    """Copy one stream to the other until EOF; half-close the destination at the end."""
    total = 0
    try:
        while True:
            data = await reader.read(READ_CHUNK)
            if not data:
                break
            total += len(data)
            writer.write(data)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError) as e:
        log_debug(f'{name}: {type(e).__name__} after {total} bytes')
    except (GeneratorExit, asyncio.CancelledError):
        # The forward is being torn down mid-transfer (task cancelled, or garbage collected).
        # Abort the destination with an RST instead of half-closing it: a graceful EOF here would
        # lie to the peer that the transfer completed. (Also: a finally block that awaits during
        # a GC-triggered close raises "coroutine ignored GeneratorExit" and leaks the socket.)
        log_debug(f'{name}: torn down after {total} bytes, aborting')
        transport = writer.transport
        if transport is not None and not transport.is_closing():
            try:
                transport.abort()
            except OSError:
                pass
        raise
    log_debug(f'{name}: done, copied {total} bytes')
    try:
        if writer.can_write_eof():
            writer.write_eof()
    except OSError as e:
        log_debug(f'{name}: write_eof failed: {type(e).__name__}: {e}')


def close_writer_now(writer, label=''):
    """Close a StreamWriter without awaiting: works on any exit path, including the teardown of
    a cancelled or garbage-collected task, where an `await` would raise and leak the socket."""
    log_debug(f'close {label}')
    try:
        writer.close()
    except OSError:
        pass


async def forward_incoming(host_reader, host_writer, cid, rule, stats):
    """One accepted host connection: dial the VM's vsock port and bridge the two streams."""
    stats.accepted += 1
    # the id must be taken here, before the connect below (which may block the loop and let other
    # connections bump the counter): the debug trace names the two pumps of *this* connection
    conn_id = stats.accepted
    log_debug(f'fwd{conn_id}: accepted')
    try:
        sock = vsock_connect(cid, rule.guest)
    except OSError as e:
        stats.forward_error += 1
        print(f'vm-relay: {rule.bind}:{rule.host}: cannot reach guest {cid}:{rule.guest}: {e}',
              file=sys.stderr)
        close_writer_now(host_writer, f'fwd{conn_id} host')
        return
    # wrap the connected socket in streams without a second connect / getaddrinfo
    guest_reader, guest_writer = await asyncio.open_connection(sock=sock)
    if DEBUG:
        # the host (relay) vsock port: the guest driver logs connections by it, this line ties a
        # debug forward to the guest-side lines about the same connection
        log_debug(f'fwd{conn_id}: host port {sock.getsockname()[1]} dialed guest {cid}:'
                  f'{rule.guest}')
    try:
        # both directions at once; each pump ends at its EOF and half-closes the other side
        await asyncio.gather(
            pump(f'fwd{conn_id} host->guest', host_reader, guest_writer),
            pump(f'fwd{conn_id} guest->host', guest_reader, host_writer))
    except Exception as e:  # noqa: BLE001 - one broken forward must not kill the relay
        stats.forward_error += 1
        print(f'vm-relay: forward {rule.bind}:{rule.host} -> {cid}:{rule.guest} failed: {e}',
              file=sys.stderr)
    finally:
        # synchronous closes only: this must be safe even when the task is being finalized by the
        # garbage collector, where an `await` here would raise and leak both sockets
        close_writer_now(guest_writer, f'fwd{conn_id} guest')
        close_writer_now(host_writer, f'fwd{conn_id} host')
        log_debug(f'fwd{conn_id}: closed')


async def serve_guest_connection(reader, writer, cid, stats):
    """
    A vsock connection from the VM to the relay's port. Relay v0 has no protocol for
    guest-initiated traffic (the outbound destination handshake is phase 3); accept only our own
    VM's CID and hang up. The plan's loopback/handshake phases replace this body.
    """
    stats.guest_connections += 1
    address = writer.get_extra_info('peername')
    peer_cid = address[0] if isinstance(address, tuple) else None
    if peer_cid is not None and peer_cid != cid:
        print(f'vm-relay: rejecting vsock connection from cid {peer_cid} (expected {cid})',
              file=sys.stderr)
    close_writer_now(writer, 'guest conn')


def spawn(background, coro):
    """
    Start `coro` as a task the relay itself holds a strong reference to (the standard asyncio
    background-task pattern), removing it from `background` when it finishes.

    This is not a formality: `asyncio.start_server`'s own protocol keeps its task only in
    `protocol._task`, which it clears in `connection_lost` -- a forward whose peer half-closed
    early could end up referenced only by its own futures, and the cyclic garbage collector would
    finalize the pending task (`Task was destroyed but it is pending!`), closing the coroutine
    mid-transfer and corrupting the connection under the application. The relay's set is a
    reference outside that cycle, so a pending task can never be collected.
    """
    task = asyncio.ensure_future(coro)
    background.add(task)
    task.add_done_callback(background.discard)
    return task


async def run_relay(cid, rules, stats=None):
    """
    The relay's main loop: the vsock listener on 65536+CID plus one TCP listener per inbound rule.
    The host (TCP) listeners are opened first so a taken port fails the launcher immediately, then
    the vsock listener -- its "listening on vsock port" line is what gramine-vm waits for.
    """
    stats = stats if stats is not None else Stats()
    # strong references to all in-flight connection tasks (see spawn())
    background = set()

    for rule in rules:
        host_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        host_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            host_sock.bind((rule.bind, rule.host))
            host_sock.listen(LISTEN_BACKLOG)
            host_sock.setblocking(False)
        except OSError as e:
            print(f'vm-relay: cannot listen on {rule.bind}:{rule.host} for guest port'
                  f' {rule.guest}: {e}', file=sys.stderr)
            raise SystemExit(3) from e
        await asyncio.start_server(
            lambda r, w, rule=rule: spawn(
                background, forward_incoming(r, w, cid, rule, stats)),
            sock=host_sock)
        print(f'vm-relay: cid={cid} port={rule.host} -> guest {cid}:{rule.guest}'
              f' (bound to {rule.bind})', flush=True)

    vsock_sock = new_vsock_listener(cid, relay_port(cid))
    vsock_server = await asyncio.start_server(
        lambda r, w: spawn(background, serve_guest_connection(r, w, cid, stats)),
        sock=vsock_sock)
    print(f'vm-relay: cid={cid} listening on vsock port {relay_port(cid)}', flush=True)

    async with vsock_server:
        await vsock_server.serve_forever()


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gramine-vm-relay')
    ap.add_argument('--cid', type=int, required=True, help='the VM\'s guest CID')
    ap.add_argument('--inbound', action='append', default=[],
                    help='inbound rule, e.g. guest=8080:host=18080:bind=127.0.0.1 or 8080:18080')
    ap.add_argument('--config', help='TOML file with an `inbound` list')
    args = ap.parse_args(argv)

    if args.cid <= 2 or args.cid >= 0xffffffff:
        ap.error(f'invalid cid {args.cid}: must be > 2 and < 0xffffffff')
    try:
        rules = load_rules(args)
    except (ValueError, KeyError, FileNotFoundError) as e:
        print(f'vm-relay: {e}', file=sys.stderr)
        return 2

    try:
        asyncio.run(run_relay(args.cid, rules))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
