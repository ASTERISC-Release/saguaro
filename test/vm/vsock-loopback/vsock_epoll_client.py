#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""Synchronous small-request client for the guest EPOLLET echo regression."""

import os
import socket
import sys
import time


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        try:
            part = sock.recv(size - len(data))
        except TimeoutError as e:
            raise TimeoutError(f'received {len(data)} of {size} bytes') from e
        if not part:
            raise AssertionError(f'connection closed after {len(data)} of {size} reply bytes')
        data.extend(part)
    return bytes(data)


def main():
    port = int(sys.argv[1])
    requests = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    if os.environ.get('EPOLL_TRANSPORT') == 'vsock':
        family = socket.AF_VSOCK
        addr = (int(os.environ['EPOLL_CID']), port)
    else:
        family = socket.AF_INET
        addr = ('127.0.0.1', port)
    sock = socket.socket(family, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect(addr)
    for i in range(requests):
        payload = i.to_bytes(4, 'little') + bytes([i % 251]) * 60
        sock.sendall(payload)
        try:
            reply = recv_exact(sock, len(payload))
        except TimeoutError as e:
            grace = float(os.environ.get('EPOLL_TIMEOUT_GRACE', '0'))
            if grace:
                deadline = time.monotonic() + grace
                while time.monotonic() < deadline:
                    sock.settimeout(min(0.5, deadline - time.monotonic()))
                    try:
                        late_reply = recv_exact(sock, len(payload))
                        if late_reply == payload:
                            sys.exit(f'request {i} echo arrived late during {grace}s grace')
                        sys.exit(f'request {i} late echo mismatch')
                    except TimeoutError:
                        pass
                sys.exit(f'request {i} timed out ({e}); no echo during {grace}s grace')
            sys.exit(f'request {i} timed out waiting for its echo: {e}')
        if reply != payload:
            sys.exit(f'request {i} reply mismatch')
    sock.close()
    print(f'{requests} synchronous epoll echo requests passed')


if __name__ == '__main__':
    main()
