#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""Container-side UDP echo peer used by test-vm-udp.sh."""
import argparse
import socket


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ready-file', required=True)
    parser.add_argument('--bind-address', default='127.0.0.1')
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.bind_address, 0))
        with open(args.ready_file, 'w') as ready:
            ready.write(str(sock.getsockname()[1]))
        data, peer = sock.recvfrom(65535)
        sock.sendto(data, peer)
        print(f'UDP_ECHOED {len(data)}', flush=True)


if __name__ == '__main__':
    main()
