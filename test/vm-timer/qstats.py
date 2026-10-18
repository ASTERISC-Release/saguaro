#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""qstats.py SOCKET: print the KVM per-vCPU statistics of a running QEMU (QMP query-stats) as JSON."""
import json
import socket
import sys

s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
f = s.makefile("rw")


def cmd(c):
    f.write(json.dumps(c) + "\n")
    f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r


f.readline()  # greeting
cmd({"execute": "qmp_capabilities"})
print(json.dumps(cmd({"execute": "query-stats", "arguments": {"target": "vcpu"}})))
