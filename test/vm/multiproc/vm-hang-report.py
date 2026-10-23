#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Where the vCPUs of a hung saguaro-vm guest are: started with SAGUARO_VM_MONITOR=<socket>, the
guest's QEMU monitor is asked for every vCPU's registers a few times, and each RIP is named after
the VM PAL function that contains it (an address outside the PAL is LibOS or application code).
A RIP that stays in the same place across samples is a spin or a loop; HLT=1 is an idle vCPU.

Usage: vm-hang-report.py <monitor-socket> [--quit] [--samples N]
--quit ends QEMU afterwards (the launcher then reports the guest's end and its serial output)."""

import argparse, bisect, glob, os, re, socket, subprocess, time

def pal_symbols():
    prefix = os.environ.get('SAGUARO_PREFIX', os.path.expanduser('~/.local/saguaro-vm'))
    pals = glob.glob(os.path.join(prefix, 'lib', '*', 'saguaro', 'vm', 'pal'))
    if not pals:
        return [], []
    out = subprocess.run(['nm', '--defined-only', pals[0]], capture_output=True, text=True).stdout
    syms = sorted((int(a, 16), n) for a, t, n in
                  (l.split(None, 2) for l in out.splitlines() if len(l.split()) == 3)
                  if t in 'tTwW')
    return [a for a, _ in syms], [n for _, n in syms]

def name(addrs, names, rip):
    i = bisect.bisect_right(addrs, rip) - 1
    if i < 0 or rip - addrs[i] > 0x10000:
        return 'outside the PAL'
    return f'{names[i]}+0x{rip - addrs[i]:x}'

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
            rip = re.search(r'RIP=([0-9a-f]+)', cpu)
            cpl = re.search(r'CPL=(\d)', cpu)
            hlt = re.search(r'HLT=(\d)', cpu)
            cr2 = re.search(r'CR2=([0-9a-f]+)', cpu)
            if not rip:
                continue
            r = int(rip.group(1), 16)
            print(f'  CPU#{n} RIP=0x{r:x} ({name(addrs, names, r)}) CPL={cpl and cpl.group(1)} '
                  f'HLT={hlt and hlt.group(1)} CR2=0x{int(cr2.group(1), 16) if cr2 else 0:x}')
        time.sleep(0.5)
    if a.quit:
        try:
            command(a.socket, 'quit')
        except OSError:
            pass

if __name__ == '__main__':
    main()
