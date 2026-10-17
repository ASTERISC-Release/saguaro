#!/usr/bin/env python3
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Summaries of an x18-scan.py TSV: shared libraries, extension-module totals, and which libraries
(transitively, by DT_NEEDED) pull in a library that needs a rebuild. Usage: x18-report.py SCAN.tsv"""
import csv
import os
import re
import sys

rows = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
for r in rows:
    r['path'] = re.sub(r'^.*x18scan/', '', r['path'])
    r['n'] = int(r['x18_insns'])
    r['needed'] = [x for x in r['needed'].split(',') if x]
    r['base'] = os.path.basename(r['path'])
ext = [r for r in rows if 'cpython' in r['base']]
libs = [r for r in rows if r not in ext]


def key(b):  # versioned soname -> file base without the full version
    return re.sub(r'(\.so)(\.\d+)*$', r'\1', b)


by_soname = {}
for r in libs:
    by_soname.setdefault(r['base'], r)
    by_soname.setdefault(re.sub(r'(\.so\.\d+)(\.\d+)*$', r'\1', r['base']), r)
dirty = {r['path'] for r in rows if r['n']}

print('| Library | x18 insns | functions | write | read only | needs rebuild |')
print('|---|---|---|---|---|---|')
for r in sorted(libs, key=lambda r: -r['n']):
    print(f"| `{r['path']}` | {r['n']} | {r['functions']} | {r['write']} | {r['read_only']} | "
          f"{'yes' if r['n'] else 'no (passes)'} |")
print()
tot = sum(r['n'] for r in ext)
print(f'Extension modules (*.cpython-312-aarch64-linux-gnu.so): {len(ext)} scanned, '
      f'{sum(1 for r in ext if r["n"])} with x18 uses ({tot} instructions in '
      f'{sum(int(r["functions"]) for r in ext)} functions), {sum(1 for r in ext if not r["n"])} pass.')
print()
print('Pulled in by DT_NEEDED (dirty library -> files that list it):')
for r in sorted((r for r in libs if r['n']), key=lambda r: -r['n']):
    names = {r['base'], re.sub(r'(\.so\.\d+)(\.\d+)*$', r'\1', r['base'])}
    users = [x['path'] for x in rows if names & set(x['needed'])]
    ue = [u for u in users if 'cpython' in u]
    print(f"- `{r['base']}`: {len(users)} files ({len(ue)} extension modules), e.g. "
          + ', '.join(f'`{os.path.basename(u)}`' for u in users[:3]))
