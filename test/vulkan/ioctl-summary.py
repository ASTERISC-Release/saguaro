#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Summarise the DRM, dma-buf and sync_file ioctls and the mappings in an strace log.

Usage: ioctl-summary.py STRACE_LOG [STRACE_LOG ...]

The log must come from `strace -f -yy -X raw -e trace=ioctl,mmap,munmap,...` (raw request numbers,
prot and flags in hex; -yy names the file behind each fd). The request numbers are decoded with the system's kernel and
libdrm headers: the script generates a small C program that prints every DRM_IOCTL_*, DMA_BUF_*
and SYNC_IOC_* macro, compiles it with cc and runs it. For each request it
prints the name, number, count, the kind of fd it was issued on, and the size of its argument.
Then the device mappings (mmap of a render node or dma-buf fd): count, sizes, prot and flags.
"""
import collections
import os
import re
import subprocess
import sys
import tempfile

HEADERS = ["/usr/include/drm/drm.h", "/usr/include/drm/i915_drm.h",
           "/usr/include/drm/amdgpu_drm.h", "/usr/include/linux/dma-buf.h",
           "/usr/include/linux/sync_file.h"]


def ioctl_table():
    """{(type, nr, family): (name, size)}; family is "core", "i915", "amdgpu", "dma-buf", "sync"."""
    names = []
    for h in HEADERS:
        if not os.path.exists(h):
            continue
        for line in open(h):
            m = re.match(r"#define\s+((DRM_IOCTL|DMA_BUF_IOCTL|DMA_BUF_SET_NAME|SYNC_IOC)_\w+)\s+(DRM_IO|_IO)", line)
            if m:
                names.append(m.group(1))
    src = "#include <stdio.h>\n#include <stdint.h>\n#include <sys/ioctl.h>\n"
    src += "".join(f"#include <{h.replace('/usr/include/', '')}>\n" for h in HEADERS if os.path.exists(h))
    src += "int main(void){\n"
    for n in sorted(set(names)):
        src += f'#ifdef {n}\nprintf("%lx {n}\\n", (unsigned long)({n}));\n#endif\n'
    src += "return 0;}\n"
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t")
        open(c, "w").write(src)
        subprocess.run(["cc", "-w", "-o", exe, c], check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        num, name = line.split()
        num = int(num, 16)
        if name.startswith("DRM_IOCTL_I915_"):
            fam = "i915"
        elif name.startswith("DRM_IOCTL_AMDGPU_"):
            fam = "amdgpu"
        elif name.startswith("DRM_IOCTL_"):
            fam = "core"
        elif name.startswith("DMA_BUF"):
            fam = "dma-buf"
        else:
            fam = "sync"
        table.setdefault(((num >> 8) & 0xff, num & 0xff, fam), (name, (num >> 16) & 0x3fff))
    return table


def node_drivers():
    """{"renderD128": "i915", ...} from /sys/class/drm."""
    res = {}
    base = "/sys/class/drm"
    if os.path.isdir(base):
        for n in os.listdir(base):
            try:
                res[n] = os.path.basename(os.readlink(os.path.join(base, n, "device", "driver")))
            except OSError:
                pass
    return res


def decode(table, drivers, req, fdtext):
    typ, nr, size = (req >> 8) & 0xff, req & 0xff, (req >> 16) & 0x3fff
    fams = ["core", "dma-buf", "sync"]
    m = re.search(r"(renderD\d+|card\d+)", fdtext)
    drv = drivers.get(m.group(1), "") if m else ""
    if typ == ord("d") and 0x40 <= nr < 0xa0:
        fams = ["i915" if drv == "i915" else "amdgpu" if drv == "amdgpu" else drv]
    for f in fams:
        hit = table.get((typ, nr, f))
        if hit:
            name, hsize = hit
            return name if hsize == size else f"{name} (size {size}, header {hsize})"
    return "?"


def fd_kind(fdtext):
    if "renderD" in fdtext or "/dev/dri/card" in fdtext:
        return "render"
    if "dmabuf" in fdtext:
        return "dmabuf"
    if "sync_file" in fdtext:
        return "sync_file"
    if "syncobj" in fdtext:
        return "syncobj"
    return fdtext[:30]


def main():
    table = ioctl_table()
    drivers = node_drivers()
    io = collections.Counter()
    io_name = {}
    io_kind = collections.defaultdict(set)
    maps = collections.Counter()
    other = collections.Counter()
    for path in sys.argv[1:]:
        for line in open(path, errors="replace"):
            m = re.search(r"ioctl\((\d+)<(.*?)>+, (0x[0-9a-f]+)", line)
            if m:
                req = int(m.group(3), 16)
                kind = fd_kind(m.group(2))
                node = re.search(r"(renderD\d+)", m.group(2))
                key = (req, drivers.get(node.group(1), "") if node else kind)
                io[key] += 1
                io_kind[key].add(kind)
                io_name[key] = decode(table, drivers, req, m.group(2))
                continue
            m = re.search(r"mmap\((\w+), (\d+), ([^,]+), ([^,]+), (\d+)<(.*?)>+, (0x[0-9a-f]+|\d+)\)", line)
            if m:
                kind = fd_kind(m.group(6))
                if kind in ("render", "dmabuf", "sync_file"):
                    maps[(kind, int(m.group(2)), m.group(3), m.group(4))] += 1
                continue
            m = re.search(r"\b(memfd_create|eventfd2|clone3?|munmap)\(", line)
            if m:
                other[m.group(1)] += 1
    print(f"{'request':<58} {'number':>10} {'count':>7} {'argsz':>5}  fd (driver)")
    for key, n in sorted(io.items(), key=lambda kv: -kv[1]):
        req, drv = key
        size = (req >> 16) & 0x3fff
        print(f"{io_name[key]:<58} {req:#10x} {n:>7} {size:>5}  {','.join(sorted(io_kind[key]))} ({drv})")
    print()
    print("device mappings (fd kind, length, prot, flags): count")
    by_kind = collections.Counter()
    sizes = collections.defaultdict(list)
    for (kind, length, prot, flags), n in sorted(maps.items(), key=lambda kv: -kv[1])[:25]:
        print(f"  {kind:<9} {length:>12} {prot:<22} {flags:<22} {n}")
    for (kind, length, prot, flags), n in maps.items():
        by_kind[kind] += n
        sizes[kind] += [length] * n
    for kind in by_kind:
        s = sizes[kind]
        print(f"  total {kind}: {by_kind[kind]} mappings, {sum(s) >> 20} MiB, largest {max(s) >> 10} KiB")
    print()
    print("other calls:", dict(other))


if __name__ == "__main__":
    main()
