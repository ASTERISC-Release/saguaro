#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Every PAL thread of a running (or hung) saguaro-vm guest, from outside, without a guest change:
the guest is started with SAGUARO_VM_MONITOR=<socket>; this script asks the QEMU monitor to open
its gdb stub, attaches gdb in batch mode, walks the VM PAL's g_thread_list and prints, per thread,
its state (STOPPED/RUNNABLE/BLOCKED/RUNNING), the PAL wait word it blocks on, whether it is a PAL helper,
its user RIP at the last kernel entry and -- for threads of the address space the stopped vCPU
runs in -- the LibOS tid and the system call it is inside (number and first two arguments, e.g.
futex and its address). Threads of other processes show "(other address space)" for the LibOS part.

Usage: vm-thread-dump.py <monitor-socket> <pal-elf> [--port N]
  pal-elf: the installed VM PAL kernel, e.g. $SAGUARO_PREFIX/lib/x86_64-linux-gnu/saguaro/vm/pal

The structure offsets below were measured on 2026-10-17 against this tree's headers (an offsets
object compiled with the PAL's and the LibOS's own flags; see FINDINGS.md of that day). A change of
struct thread, pal_tcb_vm, PAL_TCB, libos_tcb, libos_thread or PAL_CONTEXT needs them re-measured.
"""
import argparse, os, socket, subprocess, sys, tempfile, time

OFF = dict(
    thread_list_next=0x0, thread_state=0x10, thread_blocked_on=0x18, thread_is_helper=0x20,
    thread_address_space=0x28, thread_user_rip=0xe8, thread_size=0x2e8,
    paltcb_kernel_thread=0x118, paltcb_libos_tcb=0x10,
    ltcb_tp=0x10, ltcb_regs=0x20, ltcb_syscall_nr=0x28, lthread_tid=0x18,
    ctx_rdi=0x40, ctx_rsi=0x48, ctx_rax=0x68, ctx_rsp=0x78, ctx_rip=0x80,
)
STATE = {0: 'STOPPED', 1: 'RUNNABLE', 2: 'BLOCKED', 3: 'RUNNING'}

GDB_SCRIPT = r'''
python
import gdb
OFF = %(off)r
STATE = %(state)r
def rd(addr, size=8):
    try:
        b = gdb.selected_inferior().read_memory(addr, size).tobytes()
        return int.from_bytes(b, 'little')
    except Exception:
        return None
head = int(gdb.parse_and_eval('(unsigned long)&g_thread_list'))
first = rd(head)
print('g_thread_list first = %%#x' %% (first or 0))
t = first
n = 0
while t and n < 512:
    state = rd(t + OFF['thread_state'], 4)
    blocked = rd(t + OFF['thread_blocked_on'])
    helper = rd(t + OFF['thread_is_helper'], 1)
    aspace = rd(t + OFF['thread_address_space'])
    urip = rd(t + OFF['thread_user_rip'])
    line = 'thread %%#x state=%%s blocked_on=%%#x helper=%%d aspace=%%#x user_rip=%%#x' %% (
        t, STATE.get(state, state), blocked or 0, helper or 0, aspace or 0, urip or 0)
    if not helper:
        tcb = t - OFF['paltcb_kernel_thread']
        ltcb = rd(tcb + OFF['paltcb_libos_tcb'])
        if ltcb:
            tp = rd(ltcb + OFF['ltcb_tp'])
            regs = rd(ltcb + OFF['ltcb_regs'])
            nr = rd(ltcb + OFF['ltcb_syscall_nr'])
            tid = rd(tp + OFF['lthread_tid'], 4) if tp else None
            if tid is None and tp is None and regs is None:
                line += ' (other address space)'
            else:
                line += ' tid=%%s syscall_nr=%%s' %% (tid, nr)
                if regs:
                    rax = rd(regs + OFF['ctx_rax']); rdi = rd(regs + OFF['ctx_rdi'])
                    rsi = rd(regs + OFF['ctx_rsi']); rip = rd(regs + OFF['ctx_rip'])
                    line += ' in syscall rax=%%s rdi=%%#x rsi=%%#x rip=%%#x' %% (
                        rax, rdi or 0, rsi or 0, rip or 0)
                else:
                    line += ' (not in a syscall)'
    print(line)
    nxt = rd(t + OFF['thread_list_next'])
    n += 1
    if not nxt or nxt == first:
        break
    t = nxt
print('%%d threads' %% n)
end
'''


def monitor(path, cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(path)
    time.sleep(0.2)
    try:
        s.recv(65536)
    except Exception:
        pass
    s.sendall((cmd + '\n').encode())
    time.sleep(0.5)
    out = b''
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
            if b'(qemu)' in chunk:
                break
    except Exception:
        pass
    s.close()
    return out.decode(errors='replace')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('socket')
    ap.add_argument('pal')
    ap.add_argument('--port', type=int, default=12345)
    a = ap.parse_args()
    print(monitor(a.socket, 'gdbserver tcp::%d' % a.port).strip().splitlines()[-1:])
    script = GDB_SCRIPT % dict(off=OFF, state=STATE)
    with tempfile.NamedTemporaryFile('w', suffix='.gdb', delete=False) as f:
        f.write(script)
        path = f.name
    cmd = ['gdb', '-batch', '-nx', '-ex', 'set pagination off', '-ex', 'set confirm off',
           '-ex', 'file %s' % a.pal, '-ex', 'target remote :%d' % a.port, '-x', path,
           '-ex', 'detach']
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    for line in r.stdout.splitlines():
        if line.startswith(('thread 0x', 'g_thread_list', '%')) or line.endswith('threads'):
            print(line)
    err = [l for l in r.stderr.splitlines() if 'warning' not in l.lower()]
    if r.returncode != 0 or err:
        print('gdb rc %d: %s' % (r.returncode, ' | '.join(err[-3:])), file=sys.stderr)
    os.unlink(path)
    # gdb's detach leaves the stub open but the guest running; the monitor can close it
    monitor(a.socket, 'gdbserver none')


if __name__ == '__main__':
    main()
