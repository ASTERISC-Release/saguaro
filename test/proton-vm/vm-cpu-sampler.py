#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Samples every vCPU of a saguaro-vm guest through its QEMU monitor (SAGUARO_VM_MONITOR=<socket>)
at a fixed interval and prints one line per sample: the time since start, and per CPU its CPL and
the PAL symbol at RIP (or "user:<rip>" when CPL=3 — the live RIP, exact for a spinning thread; "?" outside the PAL) with the low digits of its CR3
(the address space it serves: the PAL's boot root for helpers and idle, a process root otherwise). A monitor `info registers -a`
pauses the vCPUs for microseconds only, so the guest's timing survives, unlike a gdb attach.
Usage: vm-cpu-sampler.py <monitor-socket> <pal-elf> [--interval-ms 20] [--seconds 120] > samples.txt
"""
import argparse, bisect, re, socket, subprocess, sys, time


def pal_symbols(path):
    out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
    addrs, names = [], []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 3 and p[1] in 'tTwW':
            addrs.append(int(p[0], 16)); names.append(p[2])
    return addrs, names


def name(addrs, names, rip):
    i = bisect.bisect_right(addrs, rip) - 1
    if i < 0 or rip - addrs[i] > 0x100000:
        return '?'
    return '%s+0x%x' % (names[i], rip - addrs[i])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('socket'); ap.add_argument('pal')
    ap.add_argument('--interval-ms', type=float, default=20); ap.add_argument('--seconds', type=float, default=120)
    a = ap.parse_args()
    addrs, names = pal_symbols(a.pal)
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(2); s.connect(a.socket); break
        except OSError:
            time.sleep(0.2)
    time.sleep(0.3)
    try:
        s.recv(65536)
    except Exception:
        pass
    t0 = time.monotonic()
    while time.monotonic() - t0 < a.seconds:
        t = time.monotonic()
        try:
            s.sendall(b'info registers -a\n')
            buf = b''
            while b'(qemu)' not in buf[-12:]:
                c = s.recv(65536)
                if not c:
                    return
                buf += c
        except Exception:
            break
        txt = buf.decode(errors='replace')
        cols = []
        for cpu in re.split(r'\nCPU#', '\n' + txt)[1:]:
            rip = re.search(r'RIP=([0-9a-f]+)', cpu); cpl = re.search(r'CPL=(\d)', cpu)
            cr3 = re.search(r'CR3=([0-9a-f]+)', cpu)
            if not rip or not cpl:
                continue
            r = int(rip.group(1), 16); c = int(cpl.group(1))
            where = ('user:%x' % r) if c == 3 else name(addrs, names, r)
            # the address space (CR3) says which process the CPU serves; the PAL's own root is
            # the boot root (helpers, idle), a process root is anything else
            cols.append('%s@%s' % (where, cr3.group(1)[-6:] if cr3 else '?'))
        sys.stdout.write('%08.3f %s\n' % (t - t0, ' | '.join(cols)))
        sys.stdout.flush()
        d = a.interval_ms / 1000 - (time.monotonic() - t)
        if d > 0:
            time.sleep(d)


if __name__ == '__main__':
    main()
