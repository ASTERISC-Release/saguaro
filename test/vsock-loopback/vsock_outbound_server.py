#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""Host-side 64-byte echo peer used by test-vm-outbound.sh."""
import argparse
import socket


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        part = conn.recv(size - len(data))
        if not part:
            return None
        data.extend(part)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ready-file', required=True)
    parser.add_argument('--requests', type=int, required=True)
    args = parser.parse_args()
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('127.0.0.1', 0))
        server.listen(1)
        port = server.getsockname()[1]
        with open(args.ready_file, 'w') as ready:
            ready.write(str(port))
        conn, _ = server.accept()
        with conn:
            for i in range(args.requests):
                data = recv_exact(conn, 64)
                if data is None:
                    raise RuntimeError(f'client closed after {i} requests')
                conn.sendall(data)
        print(f'ECHOED {args.requests}', flush=True)


if __name__ == '__main__':
    main()
