#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Where the vCPUs of a hung AArch64 gramine-vm guest are (the AArch64 port of the x86_64 tool on
the dev-sig-hang branch). Start the guest with GRAMINE_VM_MONITOR=<socket>; this asks the QEMU
monitor for every vCPU's registers a few times and names each PC (and ELR_EL1, the address an
exception at EL1 will return to) after the VM PAL function that contains it; an address outside
the PAL is LibOS or application code. A PC that stays in the same place across samples is a spin
or a loop; a vCPU in `wfi` (thread_idle_run) is idle.

Usage: vm-hang-report.py <monitor-socket> [--quit] [--samples N]
--quit ends QEMU afterwards (the launcher then reports the guest's end and its console output).
GRAMINE_PREFIX selects the install whose `pal` is symbolized (default ~/.local/gramine-vm)."""

import argparse
import bisect
import glob
import os
import re
import socket
import subprocess
import time


def pal_symbols():
    prefix = os.environ.get('GRAMINE_PREFIX', os.path.expanduser('~/.local/gramine-vm'))
    pals = glob.glob(os.path.join(prefix, 'lib', '*', 'gramine', 'vm', 'pal'))
    if not pals:
        return [], []
    out = subprocess.run(['nm', '--defined-only', pals[0]], capture_output=True,
                         text=True).stdout
    syms = sorted((int(a, 16), n) for a, t, n in
                  (l.split(None, 2) for l in out.splitlines() if len(l.split()) == 3)
                  if t in 'tTwW')
    return [a for a, _ in syms], [n for _, n in syms]


def name(addrs, names, pc):
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0 or pc - addrs[i] > 0x10000:
        return 'outside the PAL'
    return f'{names[i]}+0x{pc - addrs[i]:x}'


def command(path, cmd):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    s.connect(path)
    data = b''
    try:
        data += s.recv(65536)              # the banner
        s.sendall(cmd.encode() + b'\n')
        end = time.time() + 3
        while time.time() < end:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if data.rstrip().endswith(b'(qemu)') and data.count(b'(qemu)') >= 2:
                break
    finally:
        s.close()
    return data.decode(errors='replace')


def reg(text, regname):
    m = re.search(r'\b' + regname + r'\s*=\s*([0-9a-fA-F]+)', text)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('socket')
    ap.add_argument('--quit', action='store_true')
    ap.add_argument('--samples', type=int, default=3)
    a = ap.parse_args()
    addrs, names = pal_symbols()
    for k in range(a.samples):
        try:
            regs = command(a.socket, 'info registers -a')
        except OSError as e:
            print(f'hang report: no QEMU monitor at {a.socket}: {e}')
            return
        print(f'hang report, sample {k + 1}:')
        for cpu in re.split(r'\nCPU#', '\n' + regs)[1:]:
            n = cpu.split(None, 1)[0]
            pc = reg(cpu, 'PC')
            if pc is None:
                continue
            pstate = re.search(r'PSTATE=([0-9a-f]+)\s+(\S+)\s+(\S+)', cpu)
            el = pstate.group(3) if pstate else '?'
            line = f'  CPU#{n} PC=0x{pc:x} ({name(addrs, names, pc)}) {el}'
            for extra in ('ELR_EL1', 'ESR_EL1', 'FAR_EL1', 'SP_EL0', 'SP_EL1', 'X30'):
                v = reg(cpu, extra)
                if v is not None:
                    line += f' {extra}=0x{v:x}'
                    if extra in ('ELR_EL1', 'X30'):
                        line += f' ({name(addrs, names, v)})'
            print(line)
        time.sleep(0.5)
    if a.quit:
        try:
            command(a.socket, 'quit')
        except OSError:
            pass


if __name__ == '__main__':
    main()
