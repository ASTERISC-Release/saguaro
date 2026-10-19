#!/usr/bin/env python3
"""Gramine phase-0 spike (step h): write the two PT_LOAD segments of app.elf as text.bin (padded to
a fixed 64 KiB so that the enclave image layout does not depend on the program) and data.bin
(padded to its memory size), and check that the program needs no relocation."""
TEXT_PAD = 0x10000
import struct, sys

elf = open(sys.argv[1], 'rb').read()
assert elf[:4] == b'\x7fELF' and elf[4] == 2 and struct.unpack_from('<H', elf, 18)[0] == 183, 'not ELF64 AArch64'
e_entry, e_phoff, e_shoff = struct.unpack_from('<QQQ', elf, 24)
e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from('<HHHH', elf, 54)
loads = []
for i in range(e_phnum):
    p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, e_phoff + i * e_phentsize)
    if p_type == 1:
        loads.append((p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
    print(f'phdr type={p_type} flags={p_flags} off=0x{p_offset:x} vaddr=0x{p_vaddr:x} filesz=0x{p_filesz:x} memsz=0x{p_memsz:x}')
for i in range(e_shnum):
    sh_type = struct.unpack_from('<I', elf, e_shoff + i * e_shentsize + 4)[0]
    assert sh_type not in (4, 9), 'the program has relocation sections'
assert len(loads) == 2, 'expected two PT_LOAD segments'
(tf, to, tv, tfs, tms), (df, do, dv, dfs, dms) = loads
assert tf == 5 and to == 0 and tv == 0 and df == 6, 'unexpected segment layout'
assert tfs <= TEXT_PAD
open('text.bin', 'wb').write(elf[0:tfs].ljust(TEXT_PAD, b'\0'))
open('data.bin', 'wb').write(elf[do:do + dfs].ljust((dms + 4095) & ~4095, b'\0'))
print(f'entry=0x{e_entry:x} text.bin={TEXT_PAD} bytes data.bin={(dms + 4095) & ~4095} bytes at +0x{dv:x}')
