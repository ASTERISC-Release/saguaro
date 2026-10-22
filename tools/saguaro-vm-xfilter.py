#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""A relay for the guest's X connection that hides X extensions from the guest.

saguaro-vm relays the application's X connection (vsock port 6000+N for DISPLAY "2:N") to the
host's X socket with socat, which carries bytes only. A DRI3 server (Xwayland) makes Mesa's GLX
and Vulkan WSI open the GPU through that connection (DRI3Open), whose reply carries a file
descriptor: it never arrives through the relay and the guest hangs waiting for it (winex11's
OpenGL initialisation under Wine, dev docs: proton-vm/FINDINGS.md 2026-10-16). This relay
answers the guest's QueryExtension for the named extensions (default: DRI3 and Present) with
"absent", so Mesa takes its paths without them; everything else passes through unchanged.

Usage: saguaro-vm-xfilter.py --port 6001 --x-socket /tmp/.X11-unix/X0 [--hide DRI3,Present]
       (listens on AF_VSOCK, any CID; one asyncio task pair per connection)
"""

import argparse
import asyncio
import socket
import struct
import sys

VMADDR_CID_ANY = 0xFFFFFFFF
QUERY_EXTENSION = 98
GENERIC_EVENT = 35


class Connection:
    def __init__(self, hide):
        self.hide = hide
        self.little = True
        self.seq = 0               # requests sent so far (the next request gets seq + 1)
        self.rewrite = set()       # sequence numbers (16 bits) of QueryExtension to answer "absent"
        self.big_requests = False  # BIG-REQUESTS enabled by the client (seen through its request)
        self.big_opcode = None

    def u16(self, b, off):
        return struct.unpack_from('<H' if self.little else '>H', b, off)[0]

    def u32(self, b, off):
        return struct.unpack_from('<I' if self.little else '>I', b, off)[0]

    # ---- client -> server -------------------------------------------------------------------
    async def client_setup(self, reader, writer):
        head = await reader.readexactly(12)
        self.little = head[0:1] == b'l'
        n_auth = self.u16(head, 6)
        d_auth = self.u16(head, 8)
        rest = await reader.readexactly(((n_auth + 3) & ~3) + ((d_auth + 3) & ~3))
        writer.write(head + rest)
        await writer.drain()

    async def client_loop(self, reader, writer):
        try:
            await self.client_setup(reader, writer)
            while True:
                head = await reader.readexactly(4)
                opcode = head[0]
                length = self.u16(head, 2) * 4
                if length == 0:
                    # BIG-REQUESTS: a 32-bit length follows the header
                    ext = await reader.readexactly(4)
                    length = self.u32(ext, 0) * 4
                    head += ext
                body = await reader.readexactly(length - len(head)) if length > len(head) else b''
                self.seq = (self.seq + 1) & 0xFFFF
                if opcode == QUERY_EXTENSION and len(body) >= 4:
                    n = self.u16(body, 0)
                    name = body[4:4 + n].decode('latin-1', 'replace')
                    if name in self.hide:
                        self.rewrite.add(self.seq)
                writer.write(head + body)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            try:
                writer.close()
            except Exception:
                pass

    # ---- server -> client -------------------------------------------------------------------
    async def server_setup(self, reader, writer):
        head = await reader.readexactly(8)
        length = self.u16(head, 6) * 4
        rest = await reader.readexactly(length)
        writer.write(head + rest)
        await writer.drain()

    async def server_loop(self, reader, writer):
        try:
            await self.server_setup(reader, writer)
            while True:
                unit = await reader.readexactly(32)
                kind = unit[0]
                if kind == 1:  # reply: extra length in words at 4..7, sequence at 2..3
                    extra = self.u32(unit, 4) * 4
                    rest = await reader.readexactly(extra) if extra else b''
                    seq = self.u16(unit, 2)
                    if seq in self.rewrite:
                        self.rewrite.discard(seq)
                        unit = unit[:8] + b'\x00' + unit[9:]  # present = 0
                    writer.write(unit + rest)
                elif kind & 0x7F == GENERIC_EVENT:
                    extra = self.u32(unit, 4) * 4
                    rest = await reader.readexactly(extra) if extra else b''
                    writer.write(unit + rest)
                else:  # error or event: 32 bytes
                    writer.write(unit)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            try:
                writer.close()
            except Exception:
                pass


async def handle(client_reader, client_writer, x_socket, hide):
    try:
        server_reader, server_writer = await asyncio.open_unix_connection(x_socket)
    except OSError as e:
        print(f'xfilter: cannot connect to {x_socket}: {e}', file=sys.stderr)
        client_writer.close()
        return
    conn = Connection(hide)
    await asyncio.gather(conn.client_loop(client_reader, server_writer),
                         conn.server_loop(server_reader, client_writer))


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True, help='vsock port to listen on (6000 + N)')
    ap.add_argument('--x-socket', required=True, help="the host X server's unix socket")
    ap.add_argument('--hide', default='DRI3,Present', help='extensions to report as absent')
    args = ap.parse_args()
    hide = set(x for x in args.hide.split(',') if x)
    sock = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((VMADDR_CID_ANY, args.port))
    sock.listen(16)
    sock.setblocking(False)
    server = await asyncio.start_server(lambda r, w: handle(r, w, args.x_socket, hide), sock=sock)
    print(f'xfilter: vsock port {args.port} -> {args.x_socket}, hiding {sorted(hide)}', file=sys.stderr, flush=True)
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
