#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Write a minimal initrd (newc cpio, uncompressed) for the SEV launch smoke tests.

The initrd holds a static busybox and an /init that prints what the guest kernel reports about
memory encryption (the "Memory Encryption Features active" line, the SEV lines of dmesg, and
whether /dev/sev-guest exists), then powers the guest off. No cpio tool is needed.

Usage: make-smoke-initrd.py <busybox-static binary> <output initrd> [extra file dest=src ...]
"""

import os
import stat
import sys

INIT = b"""#!/bin/busybox sh
/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
echo "SMOKE: kernel $(uname -r), $(nproc) cpus, $(grep MemTotal /proc/meminfo)"
dmesg | grep -i -E 'memory encryption|SEV|sev-guest|snp' | sed 's/^/SMOKE-DMESG: /'
for m in /lib/modules/*.ko; do [ -e "$m" ] && insmod "$m" && echo "SMOKE: insmod $m ok"; done
[ -e /dev/sev-guest ] && echo "SMOKE: /dev/sev-guest present" || echo "SMOKE: no /dev/sev-guest"
mount -t configfs configfs /sys/kernel/config 2>/dev/null
R=/sys/kernel/config/tsm/report/r0
if [ -d /sys/kernel/config/tsm/report ] && mkdir $R 2>/dev/null; then
    # 64 bytes of report data: the ASCII string below, zero padded
    printf 'gramine-sev smoke test' | dd of=$R/inblob bs=64 conv=sync 2>/dev/null
    echo "SMOKE: tsm provider $(cat $R/provider), generation $(cat $R/generation)"
    if dd if=$R/outblob of=/tmp/report.bin 2>/dev/null && [ -s /tmp/report.bin ]; then
        echo "SMOKE: report $(wc -c < /tmp/report.bin) bytes"
        hexdump -v -e '32/1 "%02x" "\\n"' /tmp/report.bin | sed 's/^/SMOKE-REPORT-HEX: /'
    else
        echo "SMOKE: reading the report failed"
    fi
fi
echo "SMOKE: done"
poweroff -f
"""


class Cpio:
    def __init__(self):
        self.data = bytearray()
        self.ino = 1

    def add(self, name, mode, body=b""):
        name_b = name.encode() + b"\0"
        hdr = "070701" + "".join(f"{v:08x}" for v in (
            self.ino, mode, 0, 0, 1, 0, len(body), 0, 0, 0, 0, len(name_b), 0))
        self.ino += 1
        self.data += hdr.encode() + name_b
        self.data += b"\0" * (-len(self.data) % 4)
        self.data += body
        self.data += b"\0" * (-len(self.data) % 4)

    def dir(self, name):
        self.add(name, stat.S_IFDIR | 0o755)

    def file(self, name, body, perm=0o755):
        self.add(name, stat.S_IFREG | perm, body)

    def finish(self):
        self.add("TRAILER!!!", 0)
        self.data += b"\0" * (-len(self.data) % 512)
        return bytes(self.data)


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    busybox, out = sys.argv[1], sys.argv[2]
    c = Cpio()
    for d in ("bin", "dev", "proc", "sys", "tmp", "lib", "lib/modules"):
        c.dir(d)
    with open(busybox, "rb") as f:
        c.file("bin/busybox", f.read())
    c.file("init", INIT)
    for spec in sys.argv[3:]:
        dest, src = spec.split("=", 1)
        with open(src, "rb") as f:
            c.file(dest.lstrip("/"), f.read())
    with open(out, "wb") as f:
        f.write(c.finish())
    print(f"{out}: {os.path.getsize(out)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
