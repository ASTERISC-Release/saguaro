#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""IGVM boot image and launch measurements of the AMD SEV build of the vm PAL.

Subcommands:
  build    write the IGVM file that QEMU loads with `-object igvm-cfg` (Documentation/devel/sev/
           PLAN.md D4): the PAL ELF at its link address and, for plain SEV, the reset-vector stub
           (bios) at 4 GB with the PAL's entry point patched in. The same file also carries a
           NATIVE platform (no encryption) with the same pages, for debugging the boot path.
  measure  print the launch digest a platform computes over the file (plain SEV: SHA-256 over the
           LAUNCH_UPDATE_DATA stream, the GCTX.LD of the SEV API)
  verify-sev
           check a plain-SEV LAUNCH_MEASURE value (QMP query-sev-launch-measure) against the
           digest of the file, with the TIK of the launch session (sevctl session)
  cbitpos  print the host's C-bit position (CPUID 0x8000001F EBX[5:0])

Only the Python standard library is used; the file format follows the IGVM specification
(github.com/microsoft/igvm, igvm_defs) as QEMU's backends/igvm.c reads it.
"""

import argparse
import base64
import hashlib
import hmac
import struct
import sys
import zlib

PAGE = 4096

IGVM_MAGIC = 0x4D564749  # "IGVM"
IGVM_VHT_SUPPORTED_PLATFORM = 0x1
IGVM_VHT_PAGE_DATA = 0x302

PLATFORM_NATIVE = 0x00
PLATFORM_SEV_SNP = 0x02
PLATFORM_SEV = 0x04

# compatibility masks of the platforms this file supports
MASK_SEV = 0x1
MASK_NATIVE = 0x2

ENTRY_TAG = b'GSEVPALE'


def read_elf_pages(path):
    """Returns ({page gpa: bytearray(PAGE)}, e_entry) for the PT_LOAD segments of an ELF64 file."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise SystemExit(f'{path}: not a little-endian ELF64 file')
    e_entry, e_phoff = struct.unpack_from('<QQ', data, 0x18)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x36)
    pages = {}
    for i in range(e_phnum):
        p_type, _flags, p_offset, _vaddr, p_paddr, p_filesz, p_memsz, _align = \
            struct.unpack_from('<IIQQQQQQ', data, e_phoff + i * e_phentsize)
        if p_type != 1 or not p_memsz:  # PT_LOAD
            continue
        seg = data[p_offset:p_offset + p_filesz] + bytes(p_memsz - p_filesz)
        start = p_paddr & ~(PAGE - 1)
        end = (p_paddr + p_memsz + PAGE - 1) & ~(PAGE - 1)
        for gpa in range(start, end, PAGE):
            page = pages.setdefault(gpa, bytearray(PAGE))
            lo = max(gpa, p_paddr)
            hi = min(gpa + PAGE, p_paddr + p_memsz)
            page[lo - gpa:hi - gpa] = seg[lo - p_paddr:hi - p_paddr]
    return pages, e_entry


def build_pages(pal_path, bios_path):
    """Returns a list of (gpa, data or None for a zero page, compatibility mask), sorted by gpa."""
    pal_pages, pal_entry = read_elf_pages(pal_path)
    bios_pages, _ = read_elf_pages(bios_path)
    if pal_entry >= 1 << 32:
        raise SystemExit('PAL entry point above 4 GB')
    patched = 0
    for page in bios_pages.values():
        pos = page.find(ENTRY_TAG)
        if pos >= 0:
            struct.pack_into('<I', page, pos + len(ENTRY_TAG), pal_entry)
            patched += 1
    if patched != 1:
        raise SystemExit(f'{bios_path}: expected one {ENTRY_TAG!r} slot, found {patched}')
    if set(pal_pages) & set(bios_pages):
        raise SystemExit('PAL and bios overlap')

    result = []
    for gpa, page in pal_pages.items():
        result.append((gpa, bytes(page) if any(page) else None, MASK_SEV | MASK_NATIVE))
    for gpa, page in bios_pages.items():
        result.append((gpa, bytes(page) if any(page) else None, MASK_SEV | MASK_NATIVE))
    result.sort()
    return result


def vheader(typ, payload):
    hdr = struct.pack('<II', typ, len(payload)) + payload
    return hdr + bytes(-len(hdr) % 8)


def write_igvm(pages, output):
    platforms = [
        # compatibility_mask, highest_vtl, platform_type, platform_version, shared_gpa_boundary
        struct.pack('<IBBHQ', MASK_SEV, 0, PLATFORM_SEV, 1, 0),
        struct.pack('<IBBHQ', MASK_NATIVE, 0, PLATFORM_NATIVE, 1, 0),
    ]
    headers = b''.join(vheader(IGVM_VHT_SUPPORTED_PLATFORM, p) for p in platforms)
    n_page_headers = len(pages)
    page_hdr_size = len(vheader(IGVM_VHT_PAGE_DATA, bytes(24)))
    fixed_size = 24
    data_start = fixed_size + len(headers) + n_page_headers * page_hdr_size

    file_data = bytearray()
    offsets = {}  # identical pages are stored once, as the reference writer does
    for gpa, data, mask in pages:
        if data is None:
            file_offset = 0
        elif data in offsets:
            file_offset = offsets[data]
        else:
            file_offset = data_start + len(file_data)
            offsets[data] = file_offset
            file_data += data
        # gpa, compatibility_mask, file_offset, flags, data_type (NORMAL), reserved
        headers += vheader(IGVM_VHT_PAGE_DATA,
                           struct.pack('<QIIIHH', gpa, mask, file_offset, 0, 0, 0))
    assert fixed_size + len(headers) == data_start
    total = data_start + len(file_data)
    fixed = struct.pack('<IIIIII', IGVM_MAGIC, 1, fixed_size, len(headers), total, 0)
    checksum = zlib.crc32(fixed + headers) & 0xffffffff
    fixed = struct.pack('<IIIIII', IGVM_MAGIC, 1, fixed_size, len(headers), total, checksum)
    with open(output, 'wb') as f:
        f.write(fixed + headers + file_data)


def read_igvm(path):
    """Returns the list of (gpa, data or None, mask) page directives of an IGVM file."""
    with open(path, 'rb') as f:
        blob = f.read()
    magic, version, vh_off, vh_size, total, _checksum = struct.unpack_from('<IIIIII', blob, 0)
    if magic != IGVM_MAGIC or version != 1 or total != len(blob):
        raise SystemExit(f'{path}: not an IGVM v1 file of this tool')
    pages = []
    pos = vh_off
    while pos < vh_off + vh_size:
        typ, length = struct.unpack_from('<II', blob, pos)
        payload = blob[pos + 8:pos + 8 + length]
        if typ == IGVM_VHT_PAGE_DATA:
            gpa, mask, file_offset, _flags, _dtype, _res = struct.unpack('<QIIIHH', payload)
            data = blob[file_offset:file_offset + PAGE] if file_offset else None
            pages.append((gpa, data, mask))
        pos += (8 + length + 7) & ~7
    return pages


def sev_launch_digest(pages):
    """GCTX.LD of plain SEV: SHA-256 over every LAUNCH_UPDATE_DATA'd byte, in directive order
    (QEMU passes zero pages as data too, see cgs_set_guest_state() in target/i386/sev.c)."""
    h = hashlib.sha256()
    for _gpa, data, mask in pages:
        if mask & MASK_SEV:
            h.update(data if data is not None else bytes(PAGE))
    return h.digest()


def sev_launch_measure(tik, api_major, api_minor, build, policy, digest, mnonce):
    """LAUNCH_MEASURE of the SEV API: HMAC-SHA256 under the TIK."""
    msg = bytes([0x04, api_major, api_minor, build]) + struct.pack('<I', policy) + digest + mnonce
    return hmac.new(tik, msg, hashlib.sha256).digest()


def host_cbitpos():
    import ctypes
    import mmap
    # cpuid(eax=0x8000001f); return ebx & 0x3f  (x86-64 SysV: no arguments, result in eax)
    code = bytes([0x53,                          # push rbx
                  0xb8, 0x1f, 0x00, 0x00, 0x80,  # mov eax, 0x8000001f
                  0x31, 0xc9,                    # xor ecx, ecx
                  0x0f, 0xa2,                    # cpuid
                  0x89, 0xd8,                    # mov eax, ebx
                  0x83, 0xe0, 0x3f,              # and eax, 0x3f
                  0x5b,                          # pop rbx
                  0xc3])                         # ret
    buf = mmap.mmap(-1, PAGE, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    buf.write(code)
    addr = ctypes.addressof(ctypes.c_char.from_buffer(buf))
    func = ctypes.CFUNCTYPE(ctypes.c_uint32)(addr)
    value = func()
    del func
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('build')
    p.add_argument('--pal', required=True)
    p.add_argument('--bios', required=True)
    p.add_argument('--output', required=True)
    p = sub.add_parser('measure')
    p.add_argument('igvm')
    p.add_argument('--mode', choices=['sev'], default='sev')
    p = sub.add_parser('verify-sev')
    p.add_argument('igvm')
    p.add_argument('--tik', required=True, help='TIK file of the launch session (16 bytes)')
    p.add_argument('--measurement', required=True,
                   help='base64 "data" of QMP query-sev-launch-measure (measurement + mnonce)')
    p.add_argument('--api-major', type=int, required=True)
    p.add_argument('--api-minor', type=int, required=True)
    p.add_argument('--build-id', type=int, required=True)
    p.add_argument('--policy', type=lambda x: int(x, 0), required=True)
    sub.add_parser('cbitpos')
    args = parser.parse_args()

    if args.cmd == 'build':
        pages = build_pages(args.pal, args.bios)
        write_igvm(pages, args.output)
    elif args.cmd == 'measure':
        print(sev_launch_digest(read_igvm(args.igvm)).hex())
    elif args.cmd == 'verify-sev':
        blob = base64.b64decode(args.measurement)
        if len(blob) != 48:
            raise SystemExit(f'expected 48 bytes of measurement and nonce, got {len(blob)}')
        with open(args.tik, 'rb') as f:
            tik = f.read()
        digest = sev_launch_digest(read_igvm(args.igvm))
        expected = sev_launch_measure(tik, args.api_major, args.api_minor, args.build_id,
                                      args.policy, digest, blob[32:])
        print(f'launch digest:        {digest.hex()}')
        print(f'measurement (QEMU):   {blob[:32].hex()}')
        print(f'measurement expected: {expected.hex()}')
        if not hmac.compare_digest(expected, blob[:32]):
            print('MISMATCH')
            return 1
        print('OK')
    elif args.cmd == 'cbitpos':
        print(host_cbitpos())
    return 0


if __name__ == '__main__':
    sys.exit(main())
