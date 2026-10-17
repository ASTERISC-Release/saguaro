#!/usr/bin/env python3
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Count x18/w18 uses in every ELF file under the given directories (aarch64), as the packer's
x18 check does (scripts/vbs/pack.py: objdump -d, regex \\b[xw]18\\b on each instruction), but
streaming, and with a read/write classification and the DT_NEEDED graph.

Usage: x18-scan.py OUT.tsv DIR [DIR...]
Writes OUT.tsv (one row per ELF: path, size, insns, functions, writes, reads, needed) and prints
a summary. An x18 operand is a write when it is the destination of an instruction (the first
operand of data-processing and load instructions, the first two of ldp) and a read otherwise
(stores, compares, branches, base or index registers, later operands).
"""
import os
import re
import subprocess
import sys

FUNC_RE = re.compile(r'^([0-9a-f]+) <(.+)>:$')
INSN_RE = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
X18_RE = re.compile(r'\b[xw]18\b')
NOWRITE = ('st', 'cmp', 'cmn', 'tst', 'b.', 'cbz', 'cbnz', 'tbz', 'tbnz', 'bl', 'ret', 'br', 'blr', 'prfm', 'ccmp', 'ccmn',
           'dc', 'ic', 'msr', 'svc', 'hint')


def is_elf(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


def classify(text):
    """'W', 'R' or 'RW' for the x18/w18 operands of one instruction text"""
    t = text.split('//')[0].strip()
    parts = t.split(None, 1)
    op = parts[0]
    ops = re.split(r',\s*(?![^\[]*\])', parts[1]) if len(parts) > 1 else []
    if op.startswith(('stxr', 'stlxr', 'stxp', 'stlxp', 'cas', 'swp', 'ldadd', 'ldset', 'ldclr')):
        dests = 1                    # status or loaded value register first
    elif op == 'b' or op.startswith(NOWRITE):
        dests = 0                    # stores, compares, branches: all operands are read
    elif op.startswith(('ldp', 'ldnp', 'ldaxp', 'ldxp')):
        dests = 2
    else:
        dests = 1
    w = r = False
    for i, o in enumerate(ops):
        if X18_RE.search(o):
            if i < dests and not o.strip().startswith('['):
                w = True
            else:
                r = True
    if op in ('ldr', 'ldrb', 'ldrh', 'ldrsw', 'ldrsb', 'ldrsh') and any(X18_RE.search(o) and o.strip().endswith('!') for o in ops):
        w = True   # pre/post-index writeback
    return ('W' if w else '') + ('R' if r else '')


def scan(path):
    objdump = os.environ.get('OBJDUMP', 'objdump')
    p = subprocess.Popen([objdump, '-d', '--no-show-raw-insn', path], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, errors='replace')
    func, insns, funcs, wr, rd, first = '?', 0, {}, 0, 0, None
    for line in p.stdout:
        m = FUNC_RE.match(line.rstrip('\n'))
        if m:
            func = m.group(2)
            continue
        m = INSN_RE.match(line)
        if not m:
            continue
        insns += 1
        if X18_RE.search(m.group(2)):
            funcs[func] = funcs.get(func, 0) + 1
            c = classify(m.group(2))
            wr += 'W' in c
            rd += 'R' in c and 'W' not in c
            if first is None:
                first = m.group(2).split('//')[0].strip()
    p.wait()
    return insns, funcs, wr, rd, first


def needed(path):
    out = subprocess.run(['readelf', '-d', path], capture_output=True, text=True).stdout
    return re.findall(r'NEEDED.*\[(.+)\]', out)


def main():
    out, dirs = sys.argv[1], sys.argv[2:]
    rows = []
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in sorted(files):
                p = os.path.join(root, f)
                if os.path.islink(p) or not is_elf(p):
                    continue
                insns, funcs, wr, rd, first = scan(p)
                n = wr + rd
                rows.append((os.path.relpath(p, d) if len(dirs) == 1 else p, os.path.getsize(p),
                             insns, n, len(funcs), wr, rd, ','.join(needed(p)), first or ''))
                print(f'{rows[-1][0]}: {n} x18 insns in {len(funcs)} functions '
                      f'(write {wr}, read-only {rd}) of {insns}', flush=True)
                if os.environ.get('X18_LIST'):   # X18_LIST=1: name the functions
                    for k, v in sorted(funcs.items(), key=lambda kv: -kv[1])[:40]:
                        print(f'    {v:6d}  {k}')
    with open(out, 'w') as f:
        f.write('path\tsize\tinsns\tx18_insns\tfunctions\twrite\tread_only\tneeded\tfirst\n')
        for r in rows:
            f.write('\t'.join(map(str, r)) + '\n')
    bad = [r for r in rows if r[3]]
    print(f'{len(rows)} ELF files, {len(bad)} with x18 uses')


if __name__ == '__main__':
    main()
