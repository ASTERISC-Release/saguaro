#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""
memcached_client.py -- a small benchmark client for the memcached text protocol, for the
vm-network phase-2 example runs (tools/run-vm-network-examples.sh). The container has no memtier
or libmemcached, and the protocol is trivial:

    set <key> <flags> <exptime> <bytes>\\r\\n<data>\\r\\n   -> STORED\\r\\n
    get <key>\\r\\n                                         -> VALUE ...\\r\\n<data>\\r\\nEND\\r\\n

Checks the round trip byte for byte (a big value and 2000 small ones) and reports throughput.
Exit 0 on success; any protocol or data error exits 1 with a message.

Usage: memcached_client.py PORT [BIG=256] [SMALL_OPS=2000]
  BIG: big value size in KiB (memcached's default -I limit is ~1 MiB)
"""

import socket
import sys
import time


class LineReader:
    """
    A line reader over a socket that keeps what it read past the line: `recv` on a socket returns
    whatever arrived, so a reader that takes a line by recv()ing until CRLF and then throws the
    rest of the chunk away loses response data (the data bytes of a VALUE item, in particular, and
    then waits forever for bytes that were already delivered).
    """

    def __init__(self, sock):
        self.sock = sock
        self.buf = b''

    def readline(self):
        while b'\r\n' not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError('connection closed by memcached')
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b'\r\n')
        return line

    def readexactly(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError('connection closed by memcached')
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data


def main():
    port = int(sys.argv[1])
    big_kib = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    small_ops = int(sys.argv[3]) if len(sys.argv) > 3 else 2000

    s = socket.create_connection(('127.0.0.1', port), timeout=30)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    r = LineReader(s)

    s.sendall(b'version\r\n')
    line = r.readline()
    if not line.startswith(b'VERSION'):
        sys.exit(f'unexpected reply to version: {line!r}')
    print(f'memcached says {line.decode().strip()}')

    big = bytes(range(256)) * 4 * big_kib  # deterministic pattern, big_kib KiB
    t0 = time.monotonic()
    s.sendall(f'set big 0 0 {len(big)}\r\n'.encode() + big + b'\r\n')
    reply = r.readline()
    if reply != b'STORED':
        # memcached's default item size limit is 1m; a server started without `-I` refuses bigger
        sys.exit(f'set big ({len(big)} bytes) replied {reply!r} (default -I is 1m)')

    def get_value(key):
        s.sendall(b'get ' + key + b'\r\n')
        header = r.readline()
        parts = header.split()
        if parts[:2] != [b'VALUE', key]:
            sys.exit(f'unexpected get reply for {key!r}: {header!r}')
        data = r.readexactly(int(parts[3]))
        if r.readexactly(2) != b'\r\n' or r.readline() != b'END':
            sys.exit(f'get {key!r} did not end properly')
        return data

    data = get_value(b'big')
    dt = time.monotonic() - t0
    if data != big:
        sys.exit(f'big value mismatch ({len(data)} bytes)')
    mib_s = len(big) * 2 / (1024 * 1024) / dt
    print(f'big value round trip: {len(big)} bytes in {dt:.3f} s, ~{mib_s:.0f} MiB/s '
          '(set + get)')

    # small operations, like a real workload: set and get 20-byte values
    small = b'x' * 20
    t0 = time.monotonic()
    for i in range(small_ops):
        s.sendall(f'set k{i} 0 0 20\r\n'.encode() + small + b'\r\n')
        if r.readline() != b'STORED':
            sys.exit(f'set k{i} failed')
        if get_value(f'k{i}'.encode()) != small:
            sys.exit(f'get k{i} mismatch')
    dt = time.monotonic() - t0
    print(f'{small_ops} set+get pairs in {dt:.2f} s, '
          f'{2 * small_ops / dt:.0f} requests/s')

    # the data has to survive too: re-read the big value and compare
    if get_value(b'big') != big:
        sys.exit('big value changed between the two reads')
    print('data integrity: OK')
    s.close()


if __name__ == '__main__':
    main()
