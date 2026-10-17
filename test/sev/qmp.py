#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""Runs QMP commands on a QEMU QMP unix socket and prints each result as one JSON line.

Usage: qmp.py <socket> <command> [<command>...]   (e.g. query-sev query-sev-launch-measure)
"""

import json
import socket
import sys
import time


def main():
    path, commands = sys.argv[1], sys.argv[2:]
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(100):
        try:
            sock.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    else:
        raise SystemExit(f'cannot connect to {path}')
    stream = sock.makefile('rw')

    def call(cmd):
        stream.write(json.dumps({'execute': cmd}) + '\n')
        stream.flush()
        while True:
            msg = json.loads(stream.readline())
            if 'return' in msg or 'error' in msg:
                return msg

    json.loads(stream.readline())  # greeting
    call('qmp_capabilities')
    for cmd in commands:
        print(json.dumps(call(cmd).get('return', {})))
    return 0


if __name__ == '__main__':
    sys.exit(main())
