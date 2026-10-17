#!/usr/bin/env python3
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# Sets the PT_LOAD alignment of an ELF64 little-endian shared object from 64 KiB to 4 KiB (only where
# the segment offsets and addresses are congruent modulo 4 KiB, so the file stays valid). The vbs
# sysroot is linked with 4 KiB pages; a distribution library linked with 64 KiB pages (libgcc_s,
# libstdc++ on Ubuntu) makes glibc reserve and map the object at an aligned address the enclave's
# loader does not expect (Documentation/devel/vbs-enclave/apps-trials-arm64.md, libgcc_s).
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "ELF64 little-endian only"
phoff, = struct.unpack_from("<Q", d, 0x20)
phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
for i in range(phnum):
    off = phoff + i * phentsize
    p_type, = struct.unpack_from("<I", d, off)
    if p_type != 1:  # PT_LOAD
        continue
    p_offset, p_vaddr = struct.unpack_from("<QQ", d, off + 8)
    p_align, = struct.unpack_from("<Q", d, off + 48)
    if p_align > 0x1000 and (p_offset - p_vaddr) % 0x1000 == 0:
        struct.pack_into("<Q", d, off + 48, 0x1000)
open(dst, "wb").write(d)
