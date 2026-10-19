#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""Host side of the SEV memory check of test/sev/test-sev.sh: reads guest memory through the QEMU
monitor (HMP `pmemsave`) while sev_probe waits, and reports what the host can see.

Prints one `key=value` line per result:
  secret_page_plain   1 if the probe's buffer reads back as its marker (the host sees the data)
  pal_page_elf        1 if guest-physical 0x100000 (the PAL's ELF header) reads as "\\x7fELF"
  marker_hits         occurrences of the marker in all of guest RAM
  console_in_shared   1 if the probe's console line is in the shared region (virtio bounce buffers)
"""

import argparse
import os
import re
import socket
import tempfile
import time

SHARED = (0x29200000, 0x38000000)
CHUNK = 256 << 20


class Hmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit(f'cannot connect to the monitor {path}')
        self._read_prompt()

    def _read_prompt(self):
        data = b''
        while not data.endswith(b'(qemu) '):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise SystemExit('monitor closed')
            data += chunk
        return data

    def cmd(self, line):
        self.sock.sendall(line.encode() + b'\n')
        return self._read_prompt().decode(errors='replace')

    def read(self, addr, size):
        fd, path = tempfile.mkstemp(prefix='sev-memcheck-')
        os.close(fd)
        try:
            out = self.cmd(f'pmemsave {addr:#x} {size:#x} "{path}"')
            if 'Error' in out or 'invalid' in out:
                raise SystemExit(f'pmemsave failed: {out[-200:]}')
            with open(path, 'rb') as f:
                return f.read()
        finally:
            os.unlink(path)


def ram_ranges(ram_gib):
    # gramine-vm's layout: 2G below the PCI hole, the rest from 4G (tools/gramine-vm.in)
    low = min(ram_gib, 2) << 30
    ranges = [(0, low)]
    if ram_gib > 2:
        ranges.append((4 << 30, (4 << 30) + ((ram_gib - 2) << 30)))
    return ranges


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--monitor', required=True)
    parser.add_argument('--addr', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--xor5a', required=True, help='marker XOR 0x5a, hex (from the console)')
    parser.add_argument('--ram-gib', type=int, required=True)
    parser.add_argument('--console-text', default='SECRET addr=')
    args = parser.parse_args()

    marker = bytes(b ^ 0x5a for b in bytes.fromhex(args.xor5a))
    hmp = Hmp(args.monitor)

    page = hmp.read(args.addr, 4096)
    print(f'secret_page_plain={int(page == (marker * (4096 // len(marker)))[:4096])}')
    pal_elf = hmp.read(0x100000, 4096)[:4] == b'\x7fELF'
    print(f'pal_page_elf={int(pal_elf)}')

    hits = 0
    for start, end in ram_ranges(args.ram_gib):
        for addr in range(start, end, CHUNK):
            data = hmp.read(addr, min(CHUNK, end - addr))
            hits += len(re.findall(re.escape(marker), data))
    print(f'marker_hits={hits}')

    shared = hmp.read(SHARED[0], SHARED[1] - SHARED[0])
    print(f'console_in_shared={int(args.console_text.encode() in shared)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
