#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

import socket

CID = socket.VMADDR_CID_HOST   # or VMADDR_CID_ANY for guest
PORT = 9090

s = socket.socket(socket.AF_VSOCK, socket.SOCK_DGRAM)

s.bind((CID, PORT))
print(f"vsock DGRAM server bound to cid={CID} port={PORT}")

while True:
    data, (remote_cid, remote_port) = s.recvfrom(4096)
    print(f"Received {len(data)} bytes from cid={remote_cid} port={remote_port}: {data}")
