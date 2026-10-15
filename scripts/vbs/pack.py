#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
"""
Build-time packer for the Windows VBS-enclave PAL (phase 2, refinement (a') of PLAN.md D2).

For each AArch64 Linux ELF shared object (later: the LibOS, the patched glibc, the application and
its libraries) it builds the object's *load image* -- every PT_LOAD placed at its p_vaddr, file
bytes copied in, the rest (incl. .bss) left zero -- and concatenates the images, each at a 64 KiB
aligned offset, into one blob. A generated assembly file drops that blob verbatim into ONE
executable section of the signed enclave image (`.incbin`).

Keeping each object's image intact means every PC-relative text<->data distance the compiler baked
in still holds at any load base, so NO pack-time relocation and no two-pass link are needed: at run
time the PAL loader makes the writable PT_LOAD pages read-write (the one-way change phase 0 step c
allows) and applies the object's own dynamic relocations there, as Gramine's loaders already do.

It also runs the pack-time no-`svc` scan: an `svc #0` (0xd4000001) left in packed code would reach
the Secure Kernel, not Gramine (phase 0 step d), so the pack fails unless --allow-svc.

Usage:
  pack.py --out DIR NAME=path/to/object.so [NAME=path ...]
Outputs in DIR: packed.bin, pal_vbs_packed_blob.S, pal_vbs_packed_table.h
"""
import argparse
import os
import struct
import sys

PT_LOAD, PT_DYNAMIC = 1, 2
PF_X, PF_W = 1, 2
SHT_DYNSYM = 11
ALIGN = 0x10000            # AArch64 max page; each image is 64 KiB aligned inside the blob
PAGE = 0x1000
SVC0 = 0xD4000001


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


def read_u(data, off, n):
    return int.from_bytes(data[off:off + n], 'little')


class Elf:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.path, self.data = path, d
        if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1:
            sys.exit(f'{path}: not a little-endian ELF64')
        self.e_machine = read_u(d, 18, 2)
        if self.e_machine != 183:
            sys.exit(f'{path}: not AArch64')
        self.e_phoff = read_u(d, 32, 8)
        self.e_shoff = read_u(d, 40, 8)
        self.e_phentsize, self.e_phnum = read_u(d, 54, 2), read_u(d, 56, 2)
        self.e_shentsize, self.e_shnum = read_u(d, 58, 2), read_u(d, 60, 2)
        self.ph = [self._ph(i) for i in range(self.e_phnum)]
        self.sh = [self._sh(i) for i in range(self.e_shnum)]

    def _ph(self, i):
        o = self.e_phoff + i * self.e_phentsize
        k = ('type', 'flags', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'align')
        v = struct.unpack_from('<IIQQQQQQ', self.data, o)
        return dict(zip(k, v))

    def _sh(self, i):
        o = self.e_shoff + i * self.e_shentsize
        k = ('name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info', 'align', 'entsize')
        v = struct.unpack_from('<IIQQQQIIQQ', self.data, o)
        return dict(zip(k, v))

    def loads(self):
        return [p for p in self.ph if p['type'] == PT_LOAD]

    def dynamic_vaddr(self):
        for p in self.ph:
            if p['type'] == PT_DYNAMIC:
                return p['vaddr']
        return 0

    def dynsym_value(self, name):
        want = name.encode()
        for sh in self.sh:
            if sh['type'] != SHT_DYNSYM:
                continue
            strtab = self.sh[sh['link']]['offset']
            for i in range(sh['size'] // sh['entsize']):
                o = sh['offset'] + i * sh['entsize']
                st_name = read_u(self.data, o, 4)
                st_shndx = read_u(self.data, o + 6, 2)
                st_value = read_u(self.data, o + 8, 8)
                if st_shndx == 0:
                    continue
                end = self.data.index(b'\0', strtab + st_name)
                if self.data[strtab + st_name:end] == want:
                    return st_value
        return 0

    def load_image(self):
        loads = self.loads()
        if not loads:
            sys.exit(f'{self.path}: no PT_LOAD')
        lo = min(p['vaddr'] for p in loads) & ~(ALIGN - 1)
        hi = align_up(max(p['vaddr'] + p['memsz'] for p in loads), ALIGN)
        if loads[0]['offset'] != 0 or (loads[0]['vaddr'] & ~(ALIGN - 1)) != lo:
            sys.exit(f'{self.path}: first PT_LOAD must map the ELF header at the image base')
        img = bytearray(hi - lo)
        writable, execable = [], []
        for p in loads:
            s = p['vaddr'] - lo
            img[s:s + p['filesz']] = self.data[p['offset']:p['offset'] + p['filesz']]
            seg = (s, s + p['memsz'])
            (writable if p['flags'] & PF_W else execable).append(seg)
        # page-aligned writable ranges (what the loader will VirtualProtect to RW)
        wpages = [(a & ~(PAGE - 1), align_up(b, PAGE)) for a, b in writable]
        return lo, bytes(img), wpages, execable


def scan_svc(img, execable):
    hits = []
    for a, b in execable:
        for off in range(a & ~3, min(b, len(img)) - 3, 4):
            if struct.unpack_from('<I', img, off)[0] == SVC0:
                hits.append(off)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--entry', default='packed_entry')
    ap.add_argument('--allow-svc', action='store_true')
    ap.add_argument('objs', nargs='+', metavar='NAME=PATH')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    blob = bytearray()
    rows = []
    for spec in a.objs:
        name, _, path = spec.partition('=')
        if not path:
            sys.exit(f'bad argument {spec!r}: expected NAME=PATH')
        e = Elf(path)
        lo, img, wpages, execable = e.load_image()
        hits = scan_svc(img, execable)
        if hits and not a.allow_svc:
            sys.exit(f'{path}: {len(hits)} svc #0 in packed code (first at {hits[0]:#x}); '
                     f'they would reach the Secure Kernel. Refusing (--allow-svc to override).')
        off = align_up(len(blob), ALIGN)
        blob += b'\0' * (off - len(blob)) + img
        entry = e.dynsym_value(a.entry)
        rows.append(dict(name=name, offset=off, size=len(img), lo=lo,
                         entry=entry, dynamic=e.dynamic_vaddr(), wpages=wpages, svc=len(hits)))
        wr = ' '.join(f'[{x:#x},{y:#x})' for x, y in wpages)
        print(f'{name}: {len(img):#x} bytes at blob+{off:#x} lo={lo:#x} entry={entry:#x} '
              f'dynamic={e.dynamic_vaddr():#x} writable-pages {wr} svc={len(hits)}')

    open(os.path.join(a.out, 'packed.bin'), 'wb').write(blob)

    with open(os.path.join(a.out, 'pal_vbs_packed_blob.S'), 'w') as f:
        f.write('/* generated by scripts/vbs/pack.py: packed load images in ONE executable section */\n')
        f.write('    .section .gxpack,"xr"\n    .p2align 12\n')
        f.write('    .globl g_pal_vbs_packed_blob\ng_pal_vbs_packed_blob:\n')
        f.write('    .incbin "packed.bin"\n')

    with open(os.path.join(a.out, 'pal_vbs_packed_table.h'), 'w') as f:
        f.write('/* generated by scripts/vbs/pack.py */\n#pragma once\n#include <stdint.h>\n')
        f.write('struct pal_vbs_packed_wrange { uint64_t start, end; };\n')
        f.write('struct pal_vbs_packed_obj { const char* name; uint64_t offset, size, entry_vaddr, '
                'dynamic_vaddr; uint32_t nwrange; struct pal_vbs_packed_wrange wrange[4]; };\n')
        f.write(f'#define PAL_VBS_PACKED_COUNT {len(rows)}\n')
        f.write('static const struct pal_vbs_packed_obj g_pal_vbs_packed_objs[] = {\n')
        for r in rows:
            wr = ''.join(f'{{{x:#x},{y:#x}}},' for x, y in r['wpages'][:4])
            f.write(f'    {{ "{r["name"]}", {r["offset"]:#x}, {r["size"]:#x}, {r["entry"]:#x}, '
                    f'{r["dynamic"]:#x}, {len(r["wpages"])}, {{{wr}}} }},\n')
        f.write('};\n')
    print(f'blob {len(blob):#x} bytes -> {a.out}/{{packed.bin, pal_vbs_packed_blob.S, '
          f'pal_vbs_packed_table.h}}')


if __name__ == '__main__':
    main()
