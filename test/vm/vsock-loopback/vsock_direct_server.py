#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""Host-side AF_VSOCK echo peer for the manifest's legacy direct port 6000."""
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
    with socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((socket.VMADDR_CID_ANY, 6000))
        server.listen(1)
        with open(args.ready_file, 'w') as ready:
            ready.write('ready')
        print('LISTENING VSOCK *:6000', flush=True)
        conn, _ = server.accept()
        print('ACCEPTED direct VSOCK connection', flush=True)
        with conn:
            for i in range(args.requests):
                data = recv_exact(conn, 64)
                if data is None:
                    raise RuntimeError(f'client closed after {i} requests')
                conn.sendall(data)
    print(f'DIRECT_ECHOED {args.requests}', flush=True)


if __name__ == '__main__':
    main()
