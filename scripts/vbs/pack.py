#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
"""
Build-time packer for the Windows VBS-enclave PAL (phase 2, refinement (a') of PLAN.md D2).

For each AArch64 Linux ELF object (the vbs PAL proper, the LibOS, the application, the vDSO) it
builds the object's *load image* -- every PT_LOAD placed at its p_vaddr, file bytes copied in, the
rest (incl. .bss) left zero -- and concatenates the images, each at a 64 KiB aligned offset, into
one blob that a generated assembly file drops verbatim into ONE executable section of the signed
enclave image (`.gxpack`). Keeping each image intact means every PC-relative distance the compiler
baked in still holds at any 4 KiB aligned load base, so no pack-time relocation is needed: at run
time the loaders make the writable PT_LOAD pages read-write (the one-way change phase 0 step c
allows) and apply the object's own dynamic relocations there.

System calls. A VBS enclave does not trap `svc #0` (phase 0 step d) and there is no syscall-patched
AArch64 glibc, so every `svc #0` in the executable sections of an object is rewritten here into a
`b` to a per-site stub appended to the image (after its last PT_LOAD), which enters the LibOS with
its existing AArch64 contract (libos/src/arch/aarch64/syscallas.S): x16 = resume address, x17
scratch, every other register preserved. The stub reads the entry address from an 8-byte slot in a
data page after the stubs; the LibOS fills the slot at run time. An unfilled slot makes the stub
execute `brk`, which raises a catchable exception (never a branch to address 0, which would hang the
thread, blocker 4). Objects listed with --no-svc must contain no `svc` at all (the PAL, the LibOS).

x18. On Windows ARM64 x18 holds the TEB and the kernel reloads it when a preempted enclave thread
resumes (FINDINGS.md), so packed code must never keep a value of its own there: every packed object
is disassembled (llvm-objdump or objdump) and the pack fails if an instruction outside the
functions listed with --x18-allow names x18/w18. (The LibOS's syscall entry saves and restores the
application's x18, which is then always the TEB: allowed explicitly.)

Every packed file (ELF objects and data files such as the manifest) is also kept as its original
bytes in a read-only data section (`.gxfile`), from which the PAL serves file reads.

Usage:
  pack.py --out DIR [--obj KIND:NAME:SRC[:PATH]]... [--file NAME:SRC:PATH]... [--no-svc NAME]...
    KIND is pal (loaded by the PAL side) or user (mapped by the LibOS); PATH is the absolute path
    under which the enclave's file namespace offers the file.
Outputs in DIR: packed.bin, files.bin, pal_vbs_packed_blob.S, pal_vbs_packed_table.h
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

PT_LOAD, PT_DYNAMIC = 1, 2
PF_X, PF_W = 1, 2
SHT_DYNSYM = 11
SHF_EXECINSTR = 0x4
ALIGN = 0x10000            # AArch64 max page; each image is 64 KiB aligned inside the blob
PAGE = 0x1000
SVC0 = 0xD4000001
STUB_SIZE = 32
KIND_FILE, KIND_PAL, KIND_USER = 0, 1, 2
BRK_UNFILLED = 0x5653      # 'VS': brk immediate of a stub whose slot was never filled


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


def read_u(data, off, n):
    return int.from_bytes(data[off:off + n], 'little')


# ---- AArch64 encodings (checked by disassembling the output, see TESTING.md) ----

def enc_b(pc, target):
    off = target - pc
    assert off % 4 == 0 and -(1 << 27) <= off < (1 << 27), 'b out of range'
    return 0x14000000 | ((off >> 2) & 0x3FFFFFF)


def enc_adrp(rd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    assert -(1 << 20) <= imm < (1 << 20), 'adrp out of range'
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd


def enc_add_imm(rd, rn, imm12):
    return 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd


def enc_ldr_x(rt, rn, off):
    assert off % 8 == 0 and 0 <= off < 0x8000
    return 0xF9400000 | ((off // 8) << 10) | (rn << 5) | rt


def enc_cbz_x(rt, pc, target):
    off = target - pc
    return 0xB4000000 | (((off >> 2) & 0x7FFFF) << 5) | rt


def enc_br(rn):
    return 0xD61F0000 | (rn << 5)


def enc_brk(imm):
    return 0xD4200000 | ((imm & 0xFFFF) << 5)


NOP = 0xD503201F


def make_stub(stub, site, slot):
    """Stub at image offset `stub` for the svc at `site`; all offsets are image-relative, which is
    enough because the image base is 4 KiB aligned (adrp works on 4 KiB pages)."""
    resume = site + 4
    words = [
        enc_adrp(16, stub + 0, resume),          # x16 = resume address (entry contract)
        enc_add_imm(16, 16, resume & 0xFFF),
        enc_adrp(17, stub + 8, slot),            # x17 = *slot (libos_syscall_entry)
        enc_ldr_x(17, 17, slot & 0xFFF),
        enc_cbz_x(17, stub + 16, stub + 24),     # unfilled slot: trap instead of branching to 0
        enc_br(17),
        enc_brk(BRK_UNFILLED),
        NOP,
    ]
    assert len(words) * 4 == STUB_SIZE
    return struct.pack('<8I', *words)


class Elf:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.path, self.data = path, d
        if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1:
            sys.exit(f'{path}: not a little-endian ELF64')
        self.e_type = read_u(d, 16, 2)
        if read_u(d, 18, 2) != 183:
            sys.exit(f'{path}: not AArch64')
        if self.e_type != 3:
            sys.exit(f'{path}: not ET_DYN (a PIE or shared object is required)')
        self.e_entry = read_u(d, 24, 8)
        self.e_phoff = read_u(d, 32, 8)
        self.e_shoff = read_u(d, 40, 8)
        self.e_phentsize, self.e_phnum = read_u(d, 54, 2), read_u(d, 56, 2)
        self.e_shentsize, self.e_shnum = read_u(d, 58, 2), read_u(d, 60, 2)
        self.ph = [self._ph(i) for i in range(self.e_phnum)]
        self.sh = [self._sh(i) for i in range(self.e_shnum)] if self.e_shoff else []

    def _ph(self, i):
        o = self.e_phoff + i * self.e_phentsize
        k = ('type', 'flags', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'align')
        return dict(zip(k, struct.unpack_from('<IIQQQQQQ', self.data, o)))

    def _sh(self, i):
        o = self.e_shoff + i * self.e_shentsize
        k = ('name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info', 'align', 'entsize')
        return dict(zip(k, struct.unpack_from('<IIQQQQIIQQ', self.data, o)))

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

    def code_ranges(self):
        """vaddr ranges of executable sections (section headers), or of executable PT_LOADs when
        the object has no section headers"""
        r = [(s['addr'], s['addr'] + s['size']) for s in self.sh
             if s['flags'] & SHF_EXECINSTR and s['type'] != 8]  # not SHT_NOBITS
        if not r:
            r = [(p['vaddr'], p['vaddr'] + p['filesz']) for p in self.loads() if p['flags'] & PF_X]
        return r

    def load_image(self):
        loads = self.loads()
        if not loads:
            sys.exit(f'{self.path}: no PT_LOAD')
        lo = min(p['vaddr'] for p in loads) & ~(ALIGN - 1)
        if lo != 0 or loads[0]['offset'] != 0:
            sys.exit(f'{self.path}: the first PT_LOAD must map the ELF header at vaddr 0')
        end = max(p['vaddr'] + p['memsz'] for p in loads)
        img = bytearray(align_up(end, PAGE))
        for p in loads:
            if p['flags'] & PF_W and p['flags'] & PF_X:
                sys.exit(f'{self.path}: a writable and executable PT_LOAD cannot be packed')
            img[p['vaddr']:p['vaddr'] + p['filesz']] = \
                self.data[p['offset']:p['offset'] + p['filesz']]
        # strict W^X at page granularity: no page may hold both writable and executable bytes
        wpages, xpages = set(), set()
        for p in loads:
            pages = range(p['vaddr'] // PAGE, align_up(p['vaddr'] + p['memsz'], PAGE) // PAGE)
            (wpages if p['flags'] & PF_W else xpages if p['flags'] & PF_X else set()).update(pages)
        if wpages & xpages:
            sys.exit(f'{self.path}: a page holds both writable and executable bytes '
                     f'(page {min(wpages & xpages) * PAGE:#x}); link with a 64 KiB max page size')
        return img


def rewrite_svc(elf, img):
    """Replace each svc #0 in executable sections by a branch to a stub; return the grown image,
    the slot offset (0 if none) and the number of sites."""
    sites = []
    for a, b in elf.code_ranges():
        for off in range(align_up(a, 4), min(b, len(img)) - 3, 4):
            if struct.unpack_from('<I', img, off)[0] == SVC0:
                sites.append(off)
    if not sites:
        return img, 0, 0
    stubs = align_up(len(img), ALIGN)         # stubs start on a fresh 64 KiB page (executable)
    slot = align_up(stubs + len(sites) * STUB_SIZE, ALIGN)  # slot page: data, made RW at run time
    out = bytearray(img) + bytearray(slot + PAGE - len(img))
    for i, site in enumerate(sites):
        stub = stubs + i * STUB_SIZE
        out[stub:stub + STUB_SIZE] = make_stub(stub, site, slot)
        struct.pack_into('<I', out, site, enc_b(site, stub))
    return out, slot, len(sites)


def scan_svc(elf, img):
    return [off for a, b in elf.code_ranges()
            for off in range(align_up(a, 4), min(b, len(img)) - 3, 4)
            if struct.unpack_from('<I', img, off)[0] == SVC0]


INSN_RE = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
FUNC_RE = re.compile(r'^[0-9a-f]+ <(.+)>:$')
X18_RE = re.compile(r'\b[xw]18\b')


def x18_uses(path):
    """(function, address, instruction) for every instruction of an executable section that names
    x18 or w18"""
    tool = os.environ.get('OBJDUMP') or shutil.which('llvm-objdump') or shutil.which('objdump')
    if not tool:
        sys.exit('no llvm-objdump/objdump for the x18 check (set OBJDUMP, or --skip-x18-check)')
    out = subprocess.run([tool, '-d', '--no-show-raw-insn', path], check=True,
                         capture_output=True, text=True).stdout
    func, hits = '?', []
    for line in out.splitlines():
        m = FUNC_RE.match(line)
        if m:
            func = m.group(1)
            continue
        m = INSN_RE.match(line)
        if m and X18_RE.search(m.group(2).split('//')[0]):
            hits.append((func, int(m.group(1), 16), m.group(2).strip()))
    return hits


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--obj', action='append', default=[], metavar='KIND:NAME:SRC[:PATH]')
    ap.add_argument('--file', action='append', default=[], metavar='NAME:SRC:PATH')
    ap.add_argument('--no-svc', action='append', default=[], metavar='NAME')
    ap.add_argument('--x18-allow', action='append', default=[], metavar='NAME:FUNC[,FUNC...]',
                    help='functions of object NAME allowed to name x18')
    ap.add_argument('--skip-x18-check', action='store_true')
    ap.add_argument('--entry', default='pal_vbs_start packed_entry',
                    help='entry symbol names tried in order (else e_entry)')
    ap.add_argument('objs', nargs='*', metavar='NAME=PATH',
                    help='shorthand of --obj pal:NAME:PATH (phase-2 test objects)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    x18_allow = {}
    for spec in a.x18_allow:
        n, _, funcs = spec.partition(':')
        x18_allow.setdefault(n, set()).update(f for f in funcs.split(',') if f)

    specs = [s.split(':', 3) for s in a.obj]
    specs += [['pal'] + s.split('=', 1) for s in a.objs]
    blob, files, rows = bytearray(), bytearray(), []

    def add_file(src):
        nonlocal files
        off = align_up(len(files), 16)
        data = open(src, 'rb').read()
        files += b'\0' * (off - len(files)) + data
        return off, len(data)

    for spec in specs:
        if len(spec) < 3 or spec[0] not in ('pal', 'user'):
            sys.exit(f'bad --obj {":".join(spec)!r}: expected KIND:NAME:SRC[:PATH]')
        kind = KIND_PAL if spec[0] == 'pal' else KIND_USER
        name, src = spec[1], spec[2]
        path = spec[3] if len(spec) > 3 else ''
        e = Elf(src)
        if not a.skip_x18_check:
            bad = [h for h in x18_uses(src) if h[0] not in x18_allow.get(name, set())]
            if bad:
                funcs = sorted({h[0] for h in bad})
                sys.exit(f'{src}: {len(bad)} instructions use x18, the enclave TEB, in '
                         f'{len(funcs)} functions: {", ".join(funcs[:12])}'
                         f'{" ..." if len(funcs) > 12 else ""} (first: {bad[0][2]} at '
                         f'{bad[0][1]:#x}); build it with -ffixed-x18')
        img = e.load_image()
        map_size = len(img)
        nosvc = name in a.no_svc
        if nosvc:
            hits = scan_svc(e, img)
            if hits:
                sys.exit(f'{src}: {len(hits)} svc #0 (first at {hits[0]:#x}) in an object that must '
                         f'have none (--no-svc {name})')
            slot, nsvc = 0, 0
        else:
            img, slot, nsvc = rewrite_svc(e, img)
            left = scan_svc(e, img)
            if left:
                sys.exit(f'{src}: svc #0 left after rewriting at {left[0]:#x}')
        off = align_up(len(blob), ALIGN)
        blob += b'\0' * (off - len(blob)) + img
        entry = 0
        for sym in a.entry.split():
            entry = e.dynsym_value(sym)
            if entry:
                break
        entry = entry or e.e_entry
        foff, fsize = add_file(src)
        rows.append(dict(name=name, path=path, kind=kind, foff=foff, fsize=fsize, ioff=off,
                         isize=len(img), map_size=map_size, entry=entry,
                         dynamic=e.dynamic_vaddr(), slot=slot, nsvc=nsvc))
        print(f'{name}: {spec[0]} image {len(img):#x} at pack+{off:#x} (mapped {map_size:#x}) '
              f'entry={entry:#x} dynamic={e.dynamic_vaddr():#x} svc-rewritten={nsvc}'
              + (f' slot={slot:#x}' if slot else '') + (f' path={path}' if path else ''))

    for spec in a.file:
        p = spec.split(':', 2)
        if len(p) != 3:
            sys.exit(f'bad --file {spec!r}: expected NAME:SRC:PATH')
        foff, fsize = add_file(p[1])
        rows.append(dict(name=p[0], path=p[2], kind=KIND_FILE, foff=foff, fsize=fsize, ioff=0,
                         isize=0, map_size=0, entry=0, dynamic=0, slot=0, nsvc=0))
        print(f'{p[0]}: file {fsize:#x} bytes path={p[2]}')

    if not rows:
        sys.exit('nothing to pack')
    if not blob:
        blob = bytearray(PAGE)  # keep the section non-empty
    open(os.path.join(a.out, 'packed.bin'), 'wb').write(blob)
    open(os.path.join(a.out, 'files.bin'), 'wb').write(files or b'\0')

    with open(os.path.join(a.out, 'pal_vbs_packed_blob.S'), 'w') as f:
        f.write('/* generated by scripts/vbs/pack.py */\n'
                '/* packed load images: ONE executable section */\n'
                '    .section .gxpack,"xr"\n    .p2align 12\n'
                '    .globl g_pal_vbs_packed_blob\ng_pal_vbs_packed_blob:\n'
                '    .incbin "packed.bin"\n'
                '/* original bytes of the packed files: read-only data */\n'
                '    .section .gxfile,"dr"\n    .p2align 12\n'
                '    .globl g_pal_vbs_packed_files\ng_pal_vbs_packed_files:\n'
                '    .incbin "files.bin"\n')

    with open(os.path.join(a.out, 'pal_vbs_packed_table.h'), 'w') as f:
        f.write('/* generated by scripts/vbs/pack.py */\n#pragma once\n#include <stdint.h>\n'
                '#include "pal_vbs_abi.h"\n')
        f.write(f'#define PAL_VBS_PACKED_COUNT {len(rows)}\n')
        f.write(f'#define PAL_VBS_PACKED_BLOB_SIZE {len(blob):#x}ull\n')
        f.write(f'#define PAL_VBS_PACKED_FILES_SIZE {max(len(files), 1):#x}ull\n')
        f.write('static const struct pal_vbs_packed_obj g_pal_vbs_packed_objs[] = {\n')
        for r in rows:
            f.write(f'    {{ {c_str(r["name"])}, {c_str(r["path"])}, {r["kind"]}, {r["foff"]:#x}, '
                    f'{r["fsize"]:#x}, {r["ioff"]:#x}, {r["isize"]:#x}, {r["map_size"]:#x}, '
                    f'{r["entry"]:#x}, {r["dynamic"]:#x}, {r["slot"]:#x}, {r["nsvc"]} }},\n')
        f.write('};\n')
    print(f'images {len(blob):#x} bytes, files {len(files):#x} bytes -> {a.out}/'
          f'{{packed.bin, files.bin, pal_vbs_packed_blob.S, pal_vbs_packed_table.h}}')


if __name__ == '__main__':
    main()
