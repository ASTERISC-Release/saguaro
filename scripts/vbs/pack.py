#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
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

TPIDR_EL0. The kernel does not keep TPIDR_EL0 reliably for enclave threads (FINDINGS.md: after a
context switch a thread may see another enclave thread's value), so the thread pointer lives in the
thread's TEB instead: every `mrs Xt, tpidr_el0` of packed code becomes `ldr Xt, [x18, #0x28]` and
every `msr tpidr_el0, Xt` becomes `str Xt, [x18, #0x28]` (NT_TIB.ArbitraryUserPointer; x18 is
the TEB and reliable).

x18. On Windows ARM64 x18 holds the TEB and the kernel reloads it when a preempted enclave thread
resumes (FINDINGS.md), so packed code must never keep a value of its own there: every packed object
is disassembled (llvm-objdump or objdump) and the pack fails if an instruction outside the
functions listed with --x18-allow names x18/w18. (The LibOS's syscall entry saves and restores the
application's x18, which is then always the TEB: allowed explicitly.)

Exceptions. A fault in packed code is dispatched by Windows only if the faulting PC has unwind
data (vertdll has no run-time function tables, FINDINGS.md), so the generated assembly also emits
static .pdata/.xdata entries covering every executable range of every packed object (chunks of
at most 1 MB, the limit of an ARM64 .xdata function length): no unwind codes, and the
language-specific handler `pal_vbs_packed_exception_handler` of the Windows enclave runtime, which
hands the fault to the PAL.

Every packed file (ELF objects and data files such as the manifest) is also kept as its original
bytes in a read-only data section (`.gxfile`), from which the PAL serves file reads.

x86_64 (Documentation/devel/vbs-enclave/PLAN-x64.md). The objects' machine decides; all must agree.
A `syscall` returns an error in a VBS enclave instead of trapping (FINDINGS-x64.md), so every
`syscall` (found by disassembling the executable sections) becomes `ud2` -- the same two bytes, so
nothing moves -- and its offset in the blob goes into a sorted table (g_pal_vbs_syscall_sites in
pal_vbs_packed_table.h); W's exception handler sends a `ud2` at a listed site into the LibOS's
syscall entry, with rcx and r11 as `syscall` leaves them. Objects listed with --no-svc must contain
no `syscall`. Exception delivery leaves the red zone below %rsp intact (FINDINGS-x64.md), so packed
code may use it; --red-zone-check lists (and refuses) instructions that address memory below %rsp,
outside functions listed with --red-zone-allow, should that ever be needed. The x18 and TPIDR_EL0
handling is AArch64 only. The unwind data are x64 RUNTIME_FUNCTIONs, one per executable range, whose UNWIND_INFO
(no unwind codes) names the same handler.

Usage:
  pack.py --out DIR [--obj KIND:NAME:SRC[:PATH]]... [--file NAME:SRC:PATH]... [--no-svc NAME]...
    KIND is pal (loaded by the PAL side) or user (mapped by the LibOS); PATH is the absolute path
    under which the enclave's file namespace offers the file.
Enclave configuration. The packed file named "manifest" may set `vbs.max_threads` (default 4): the
number of threads the signed image allows inside the enclave at once, and `vbs.enclave_size`
(default "256M", a power of two): both are written into pal_vbs_image_config.h for the image's
IMAGE_ENCLAVE_CONFIG, from which the host takes them.

Arguments may also come from a file, one per line: pack.py @args.txt (many packed files).

Outputs in DIR: packed.bin, files.bin, pal_vbs_packed_blob.S, pal_vbs_packed_table.h,
pal_vbs_image_config.h
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tomllib

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


EM_AARCH64, EM_X86_64 = 183, 62


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
        self.machine = read_u(d, 18, 2)
        if self.machine not in (EM_AARCH64, EM_X86_64):
            sys.exit(f'{path}: neither AArch64 nor x86_64')
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


MRS_TPIDR = 0xD53BD040     # mrs Xt, tpidr_el0  (| Rt)
MSR_TPIDR = 0xD51BD040     # msr tpidr_el0, Xt  (| Rt)
TEB_TLS_OFFSET = 0x28      # NT_TIB.ArbitraryUserPointer


def rewrite_tpidr(elf, img):
    """mrs/msr TPIDR_EL0 -> ldr/str [x18, #0x28]; returns the number of sites"""
    n = 0
    for a, b in elf.code_ranges():
        for off in range(align_up(a, 4), min(b, len(img)) - 3, 4):
            w = struct.unpack_from('<I', img, off)[0]
            rt = w & 0x1F
            if w & ~0x1F == MRS_TPIDR:
                new = 0xF9400000 | ((TEB_TLS_OFFSET // 8) << 10) | (18 << 5) | rt   # ldr
            elif w & ~0x1F == MSR_TPIDR:
                new = 0xF9000000 | ((TEB_TLS_OFFSET // 8) << 10) | (18 << 5) | rt   # str
            else:
                continue
            struct.pack_into('<I', img, off, new)
            n += 1
    return n


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


def objdump_lines(path):
    """(function, address, instruction text) for every instruction of the executable sections"""
    tool = os.environ.get('OBJDUMP') or shutil.which('llvm-objdump') or shutil.which('objdump')
    if not tool:
        sys.exit('no llvm-objdump/objdump to disassemble the packed objects (set OBJDUMP)')
    out = subprocess.run([tool, '-d', '--no-show-raw-insn', path], check=True,
                         capture_output=True, text=True).stdout
    func = '?'
    for line in out.splitlines():
        m = FUNC_RE.match(line)
        if m:
            func = m.group(1)
            continue
        m = INSN_RE.match(line)
        if m:
            yield func, int(m.group(1), 16), m.group(2).split('#')[0].strip()


SYSCALL_RE = re.compile(r'^syscall\b')
REDZONE_RE = re.compile(r'-0x[0-9a-f]+\(%rsp\)')


def x86_scan(path):
    """syscall sites (vaddrs) and red-zone uses (function, vaddr, instruction) of an x86_64 object"""
    sites, redzone = [], []
    for func, addr, insn in objdump_lines(path):
        if SYSCALL_RE.match(insn):
            sites.append(addr)
        elif REDZONE_RE.search(insn) and not insn.startswith('lea'):
            redzone.append((func, addr, insn))
    return sites, redzone


def rewrite_syscall(path, img, sites):
    """syscall (0f 05) -> ud2 (0f 0b) at every site; returns the sites"""
    for v in sites:
        if img[v:v + 2] != b'\x0f\x05':
            sys.exit(f'{path}: no syscall bytes at {v:#x}')
        img[v + 1] = 0x0b
    return sites


def unwind_data_x64(ranges):
    """static .pdata/.xdata for x86_64 packed code: one RUNTIME_FUNCTION per executable range,
    UNWIND_INFO version 1, UNW_FLAG_EHANDLER, no prologue and no unwind codes"""
    xdata = ('gx_xdata_x64:\n    .byte 0x09, 0, 0, 0\n'
             '    .rva pal_vbs_packed_exception_handler\n    .long 0\n')
    pdata = ''.join(f'    .rva g_pal_vbs_packed_blob + {lo:#x}\n'
                    f'    .rva g_pal_vbs_packed_blob + {hi:#x}\n    .rva gx_xdata_x64\n'
                    for lo, hi in sorted(ranges) if hi > lo)
    return ('/* unwind data of the packed code: faults there reach W\'s exception handler */\n'
            '    .section .xdata,"dr"\n    .p2align 2\n' + xdata +
            '    .section .pdata,"dr"\n    .p2align 2\n' + pdata)


XDATA_MAX = 0x3FFFF * 4    # largest function length of an ARM64 .xdata record (18 bits, words)


def unwind_data(ranges):
    """static .pdata/.xdata for packed code: one record per chunk of at most XDATA_MAX bytes"""
    xdata, pdata, n = [], [], 0
    for lo, hi in sorted(ranges):
        lo &= ~3
        while lo < hi:
            size = min(align_up(hi - lo, 4), XDATA_MAX)
            # word 0: function length (words) | X=1 (exception handler) | E=1 (one epilog, its
            # codes at index 0) | 1 code word; the code word is `end` (0xe4) four times
            w0 = (size // 4) | (1 << 20) | (1 << 21) | (1 << 27)
            xdata.append(f'gx_xdata_{n}:\n    .long {w0:#x}\n    .long 0xe4e4e4e4\n'
                         f'    .rva pal_vbs_packed_exception_handler\n    .long 0\n')
            pdata.append(f'    .rva g_pal_vbs_packed_blob + {lo:#x}\n    .rva gx_xdata_{n}\n')
            n += 1
            lo += size
    return ('/* unwind data of the packed code: faults there reach W\'s exception handler */\n'
            '    .section .xdata,"dr"\n    .p2align 2\n' + ''.join(xdata) +
            '    .section .pdata,"dr"\n    .p2align 2\n' + ''.join(pdata))


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__, fromfile_prefix_chars='@',
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--obj', action='append', default=[], metavar='KIND:NAME:SRC[:PATH]')
    ap.add_argument('--file', action='append', default=[], metavar='NAME:SRC:PATH')
    ap.add_argument('--no-svc', action='append', default=[], metavar='NAME')
    ap.add_argument('--x18-allow', action='append', default=[], metavar='NAME:FUNC[,FUNC...]',
                    help='functions of object NAME allowed to name x18')
    ap.add_argument('--skip-x18-check', action='store_true')
    ap.add_argument('--red-zone-allow', action='append', default=[], metavar='NAME:FUNC[,FUNC...]',
                    help='x86_64: functions of object NAME allowed to address the red zone')
    ap.add_argument('--red-zone-check', action='store_true',
                    help='x86_64: refuse code that addresses memory below %%rsp')
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
    rz_allow = {}
    for spec in a.red_zone_allow:
        n, _, funcs = spec.partition(':')
        rz_allow.setdefault(n, set()).update(f for f in funcs.split(',') if f)
    machine = None
    syscall_sites = []

    specs = [s.split(':', 3) for s in a.obj]
    specs += [['pal'] + s.split('=', 1) for s in a.objs]
    blob, files, rows = bytearray(), bytearray(), []
    code_ranges = []

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
        if machine is None:
            machine = e.machine
        elif e.machine != machine:
            sys.exit(f'{src}: a different machine than the objects before it')
        if machine == EM_X86_64:
            img = e.load_image()
            map_size = len(img)
            sites, redzone = x86_scan(src)
            bad = [h for h in redzone if h[0] not in rz_allow.get(name, set())]
            if bad and a.red_zone_check:
                funcs = sorted({h[0] for h in bad})
                sys.exit(f'{src}: {len(bad)} instructions use the red zone (below %rsp) in '
                         f'{len(funcs)} functions: {", ".join(funcs[:12])}'
                         f'{" ..." if len(funcs) > 12 else ""} (first: {bad[0][2]} at '
                         f'{bad[0][1]:#x}) (--red-zone-check)')
            if name in a.no_svc and sites:
                sys.exit(f'{src}: {len(sites)} syscall (first at {sites[0]:#x}) in an object that '
                         f'must have none (--no-svc {name})')
            rewrite_syscall(src, img, sites)
            off = align_up(len(blob), ALIGN)
            blob += b'\0' * (off - len(blob)) + img
            syscall_sites += [off + v for v in sites]
            entry = 0
            for sym in a.entry.split():
                entry = e.dynsym_value(sym)
                if entry:
                    break
            entry = entry or e.e_entry
            foff, fsize = add_file(src)
            for p_ in e.loads():
                if p_['flags'] & PF_X:
                    code_ranges.append((off + p_['vaddr'], off + p_['vaddr'] + p_['filesz']))
            rows.append(dict(name=name, path=path, kind=kind, foff=foff, fsize=fsize, ioff=off,
                             isize=len(img), map_size=map_size, entry=entry,
                             dynamic=e.dynamic_vaddr(), slot=0, nsvc=len(sites)))
            print(f'{name}: {spec[0]} x86_64 image {len(img):#x} at pack+{off:#x} entry={entry:#x} '
                  f'dynamic={e.dynamic_vaddr():#x} syscall->ud2={len(sites)} red-zone-insns='
                  f'{len(redzone)}' + (f' path={path}' if path else ''))
            continue
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
        ntls = rewrite_tpidr(e, img)
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
        # executable ranges (image-relative) for the static unwind data: the code PT_LOADs and the
        # svc stubs
        for p_ in e.loads():
            if p_['flags'] & PF_X:
                code_ranges.append((off + p_['vaddr'], off + p_['vaddr'] + p_['filesz']))
        if slot:
            code_ranges.append((off + align_up(map_size, ALIGN), off + slot))
        rows.append(dict(name=name, path=path, kind=kind, foff=foff, fsize=fsize, ioff=off,
                         isize=len(img), map_size=map_size, entry=entry,
                         dynamic=e.dynamic_vaddr(), slot=slot, nsvc=nsvc))
        print(f'{name}: {spec[0]} image {len(img):#x} at pack+{off:#x} (mapped {map_size:#x}) '
              f'entry={entry:#x} dynamic={e.dynamic_vaddr():#x} svc-rewritten={nsvc} '
              f'tpidr-rewritten={ntls}'
              + (f' slot={slot:#x}' if slot else '') + (f' path={path}' if path else ''))

    max_threads = 4
    enclave_size = 0x10000000
    debug = False
    family_id = bytes(16)
    image_id = bytes(16)
    isv_svn = 0
    for spec in a.file:
        p = spec.split(':', 2)
        if len(p) != 3:
            sys.exit(f'bad --file {spec!r}: expected NAME:SRC:PATH')
        if p[0] == 'manifest':
            with open(p[1], 'rb') as mf:
                m = tomllib.load(mf)
            max_threads = m.get('vbs', {}).get('max_threads', max_threads)
            if not isinstance(max_threads, int) or not 1 <= max_threads <= 1024:
                sys.exit(f'{p[1]}: vbs.max_threads must be an integer from 1 to 1024')
            es = m.get('vbs', {}).get('enclave_size', '256M')
            mult = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}
            try:
                enclave_size = int(es[:-1]) * mult[es[-1].upper()] if isinstance(es, str) \
                    else int(es)
            except (ValueError, KeyError):
                sys.exit(f'{p[1]}: vbs.enclave_size must be like "512M" or "1G"')
            if enclave_size & (enclave_size - 1) or not 0x4000000 <= enclave_size <= 0x1000000000:
                sys.exit(f'{p[1]}: vbs.enclave_size must be a power of two from 64M to 64G')
            vbs = m.get('vbs', {})
            debug = vbs.get('debug', False)
            if not isinstance(debug, bool):
                sys.exit(f'{p[1]}: vbs.debug must be true or false')
            def id16(key):
                v = vbs.get(key, '')
                try:
                    b = bytes.fromhex(v) if isinstance(v, str) else None
                except ValueError:
                    b = None
                if b is None or len(b) > 16:
                    sys.exit(f'{p[1]}: vbs.{key} must be a hex string of at most 16 bytes')
                return b.ljust(16, b'\0')
            family_id = id16('family_id')
            image_id = id16('image_id')
            isv_svn = vbs.get('isv_svn', 0)
            if not isinstance(isv_svn, int) or not 0 <= isv_svn <= 0xffffffff:
                sys.exit(f'{p[1]}: vbs.isv_svn must be an integer from 0 to 4294967295')
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
                + (unwind_data_x64(code_ranges) if machine == EM_X86_64 else unwind_data(code_ranges)) +
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
        if machine == EM_X86_64:
            # the syscall sites rewritten into ud2, as offsets in the blob (sorted)
            sites = sorted(syscall_sites)
            assert all(x < 1 << 32 for x in sites)
            f.write(f'#define PAL_VBS_SYSCALL_SITE_COUNT {len(sites)}ull\n'
                    'static const uint32_t g_pal_vbs_syscall_sites[] = {'
                    + (', '.join(f'{x:#x}' for x in sites) if sites else '0') + '};\n')
    with open(os.path.join(a.out, 'pal_vbs_image_config.h'), 'w') as f:
        f.write('/* generated by scripts/vbs/pack.py from the manifest: the enclave configuration */\n'
                '#pragma once\n'
                f'#define PAL_VBS_THREADS {max_threads} /* vbs.max_threads */\n'
                f'#define PAL_VBS_SIZE {enclave_size:#x}ull /* vbs.enclave_size */\n'
                f'#define PAL_VBS_POLICY {1 if debug else 0} /* vbs.debug '
                '(IMAGE_ENCLAVE_POLICY_DEBUGGABLE) */\n'
                '#define PAL_VBS_FAMILY_ID { ' + ', '.join(f'{x:#04x}' for x in family_id)
                + ' } /* vbs.family_id */\n'
                '#define PAL_VBS_IMAGE_ID { ' + ', '.join(f'{x:#04x}' for x in image_id)
                + ' } /* vbs.image_id */\n'
                f'#define PAL_VBS_SVN {isv_svn} /* vbs.isv_svn */\n')
    print(f'enclave configuration: {max_threads} threads (vbs.max_threads), '
          f'{enclave_size >> 20} MiB (vbs.enclave_size), debug={str(debug).lower()} (vbs.debug), '
          f'family {family_id.hex()}, image {image_id.hex()}, svn {isv_svn}')
    print(f'images {len(blob):#x} bytes, files {len(files):#x} bytes -> {a.out}/'
          f'{{packed.bin, files.bin, pal_vbs_packed_blob.S, pal_vbs_packed_table.h}}')


if __name__ == '__main__':
    main()
