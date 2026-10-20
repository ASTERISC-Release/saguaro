#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""
vsock_host_client.py -- host side of the `vsock-loopback` regression test
(see the C file next to it and dev docs: CHANGELOG.md, phase 1).

Runs in the container (the guest's "host") against an AF_INET TCP server inside the VM, over the
guest's vsock: connects to (guest CID, port). Checks that host-initiated connections work
repeatedly: <sequential> sequential connections, then <concurrent> connections established at the
same time, then one more sequential connection to show the listener survived; data must come back
unchanged, and the listener must still answer at the end. Then tells the server to exit.

Usage: python3 vsock_host_client.py [--cid N] [--port 7000] [--sequential 100]
                                    [--concurrent 16] [--size 8192]
--cid defaults to the guest CID of the QEMU process running `vsock_echo_server` (the suite starts
it just before us); the process scan also waits for the VM to appear.
"""

import argparse
import os
import random
import re
import socket
import subprocess
import sys
import threading
import time

GOODBYE = b'GOODBYE'


def dump_sockets(when):
    """Host-side vsock socket states at a failure moment (the guest is still alive)."""
    print(f'--- ss --vsock -a at {when}:')
    try:
        out = subprocess.run(['ss', '--vsock', '-a'], capture_output=True, text=True, timeout=10)
        print(out.stdout + out.stderr)
    except Exception as e:  # noqa: BLE001 - diagnostics must never mask the real failure
        print(f'(ss failed: {e})')
    sys.stdout.flush()


def find_guest_cid(app, timeout=120):
    """CID of the single QEMU whose cmdline carries `app` in its -gramine-args fw_cfg item."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        cids = []
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                with open(f'/proc/{pid}/cmdline', 'rb') as f:
                    parts = f.read().split(b'\0')
            except OSError:
                continue
            if not any(b'qemu' in p for p in parts[:1]):
                continue
            cmdline = b' '.join(parts).decode('utf-8', 'replace')
            if f'-gramine-args init "{app}"' not in cmdline:
                continue
            m = re.search(r'guest-cid=(\d+)', cmdline)
            if m:
                cids.append(int(m.group(1)))
        if len(cids) == 1:
            return cids[0]
        if len(cids) > 1:
            sys.exit(f'found several guest VMs running {app}: cids {sorted(set(cids))}')
        time.sleep(0.5)
    sys.exit(f'no QEMU process running {app} appeared within {timeout} s')


def find_child_guest_cid(app, ancestor_pid, timeout=120):
    """
    CID of the QEMU that is a descendant of `ancestor_pid` (the `gramine-vm` this suite started),
    which is unambiguous even when other VMs of the same application run in parallel.
    """
    def ppid_of(pid):
        try:
            with open(f'/proc/{pid}/stat', 'rb') as f:
                # pid (comm) state ppid ... -- comm contains spaces and parentheses, split after
                # the last ')'; the remaining fields start with the state char, then the ppid
                rest = f.read().rsplit(b')', 1)[1].split()
            return int(rest[1])
        except (OSError, IndexError, ValueError):
            return None

    deadline = time.monotonic() + timeout
    ancestor = int(ancestor_pid)
    while time.monotonic() < deadline:
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                with open(f'/proc/{pid}/cmdline', 'rb') as f:
                    parts = f.read().split(b'\0')
            except OSError:
                continue
            if not parts or b'qemu' not in parts[0]:
                continue
            chain = int(pid)
            while chain and chain != 1:
                if chain == ancestor:
                    cmdline = b' '.join(parts).decode('utf-8', 'replace')
                    if '-gramine-args' not in cmdline:
                        # not the guest yet: the AArch64 launcher first runs a short QEMU that only
                        # dumps the machine's device tree; keep scanning for the real one
                        break
                    if f'-gramine-args init "{app}"' not in cmdline:
                        sys.exit(f'QEMU under pid {ancestor} runs a different application')
                    m = re.search(r'guest-cid=(\d+)', cmdline)
                    if not m:
                        sys.exit(f'QEMU under pid {ancestor} has no guest-cid')
                    return int(m.group(1))
                chain = ppid_of(chain)
        time.sleep(0.2)
    sys.exit(f'no QEMU descended from pid {ancestor_pid} appeared within {timeout} s')


def wait_for_listener(family, addr, timeout=120):
    """The guest boots for some seconds before the server listens; retry while it refuses.
    While the guest kernel has not brought its vsock driver up yet, the host's vsock core does
    not know this CID at all and answers ENHOSTUNREACH (No route to host); a listening socket
    that is not established yet gives ECONNREFUSED; both are expected until the server listens.
    `family`/`addr` are AF_VSOCK/(cid, port) for a direct connection, or AF_INET/('127.0.0.1',
    port) to go through the VM's relay."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s = socket.socket(family, socket.SOCK_STREAM)
        s.settimeout(10)
        try:
            s.connect(addr)
            return s
        except (ConnectionRefusedError, ConnectionResetError, OSError):
            # ECONNREFUSED / ENHOSTUNREACH / timeouts while the guest is booting
            s.close()
            time.sleep(0.5)
    sys.exit(f'nothing listens on {addr} within {timeout} s')


def one_connection(family, addr, seq, payload, read_timeout=60):
    """One request/response round trip: send seq+payload, half-close, read the echo, verify."""
    s = socket.socket(family, socket.SOCK_STREAM)
    s.settimeout(read_timeout)
    try:
        s.connect(addr)
        s.sendall(seq.to_bytes(4, 'little') + payload)
        s.shutdown(socket.SHUT_WR)
        echo = b''
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            echo += chunk
        want = seq.to_bytes(4, 'little') + payload
        if echo != want:
            raise AssertionError(
                f'seq {seq}: echo differs (got {len(echo)} bytes, want {len(want)}): '
                f'{_diff_desc(echo, want)}')
    except (OSError, AssertionError):
        dump_sockets(f'seq {seq} (its socket still open)')  # before we close it
        raise
    finally:
        s.close()


def _diff_desc(got, want):
    if not got:
        return 'got nothing'
    n = min(len(got), len(want))
    for i in range(n):
        if got[i] != want[i]:
            return f'first difference at byte {i}: got {got[i]:#x} want {want[i]:#x}'
    return f'prefix matches, length differs (got {len(got)}, want {len(want)})'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cid', type=int)
    ap.add_argument('--app', default='vsock_echo_server')
    ap.add_argument('--vm-pid', type=int,
                    help='PID of the gramine-vm launcher whose QEMU child to use (unambiguous'
                         ' when several VMs run in parallel)')
    ap.add_argument('--port', type=int, default=7000)
    ap.add_argument('--tcp', type=int,
                    help='connect over host TCP to 127.0.0.1:PORT (through the VM\'s vsock relay)'
                         ' instead of a direct AF_VSOCK connection to the guest')
    ap.add_argument('--sequential', type=int, default=100)
    ap.add_argument('--concurrent', type=int, default=16)
    ap.add_argument('--size', type=int, default=8192)
    args = ap.parse_args()

    if args.tcp is not None:
        # through the relay: a host TCP listener that the relay forwards to the guest
        family, addr = socket.AF_INET, ('127.0.0.1', args.tcp)
        print(f'relay tcp {addr[0]}:{addr[1]}', flush=True)
    else:
        if args.cid is not None:
            cid = args.cid
        elif args.vm_pid is not None:
            cid = find_child_guest_cid(args.app, args.vm_pid)
        else:
            cid = find_guest_cid(args.app)
        family, addr = socket.AF_VSOCK, (cid, args.port)
        print(f'guest cid {cid}, port {args.port}', flush=True)

    # the first working connection also proves the VM booted and the listener is up
    first = wait_for_listener(family, addr)
    first.close()

    rng = random.Random(20261012)

    def run_sequential(n, what):
        for i in range(n):
            payload = rng.randbytes(args.size)
            try:
                one_connection(family, addr, i, payload)
            except (OSError, AssertionError) as e:
                print(f'ERROR {what} connection #{i}: {type(e).__name__}: {e}', file=sys.stderr)
                dump_sockets(f'{what} failure at connection {i}')
                sys.exit(f'{what} phase failed at connection {i + 1} of {n}')
            if (i + 1) % 10 == 0 or i + 1 == n:
                print(f'{what}: {i + 1} connections ok', flush=True)

    # phase 1: many sequential connect/send/close cycles (the phase-1 bug: the second one died)
    run_sequential(args.sequential, 'sequential')

    # phase 2: `concurrent` connections established at the same time; the single-threaded server
    # keeps all but one in its backlog while it serves each, which is the accept bookkeeping we
    # want to stress. Each connection still carries its own seq, so the data checks stay sound.
    established = []
    lock = threading.Lock()
    errors = []

    def opener(i):
        try:
            s = socket.socket(family, socket.SOCK_STREAM)
            s.settimeout(60)
            s.connect(addr)
            s.sendall(i.to_bytes(4, 'little') + payloads[i])
            s.shutdown(socket.SHUT_WR)
            with lock:  # completion order is arbitrary; keep the seq with the socket
                established.append((i, s))
        except OSError as e:
            errors.append(f'connect/send #{i}: {e}')

    payloads = {i: rng.randbytes(args.size) for i in range(args.concurrent)}
    threads = [threading.Thread(target=opener, args=(i,)) for i in range(args.concurrent)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        for e in errors:
            print(f'ERROR {e}', file=sys.stderr)
        sys.exit(f'{len(errors)} of {args.concurrent} concurrent connects failed')

    # read the echoes back concurrently (the server works them off one by one)
    def reader(i, s):
        try:
            s.settimeout(120)
            echo = b''
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                echo += chunk
            want = i.to_bytes(4, 'little') + payloads[i]
            if echo != want:
                errors.append(f'concurrent seq {i}: {_diff_desc(echo, want)}')
        except OSError as e:
            errors.append(f'read concurrent #{i}: {e}')
        finally:
            s.close()

    threads = [threading.Thread(target=reader, args=(i, s)) for i, s in established]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        for e in errors:
            print(f'ERROR {e}', file=sys.stderr)
        sys.exit(f'{args.concurrent} concurrent connections: {len(errors)} failed')
    print(f'concurrent: {args.concurrent} connections ok', flush=True)

    # phase 3: the listener must still serve one more sequential connection afterwards
    one_connection(family, addr, 0, rng.randbytes(args.size))
    print(f'the listener is still healthy after {args.sequential + args.concurrent + 1} '
          'connections', flush=True)

    # tell the server to exit
    s = socket.socket(family, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(addr)
    s.sendall(GOODBYE)
    s.shutdown(socket.SHUT_WR)
    s.close()
    print('PASS', flush=True)


if __name__ == '__main__':
    main()
