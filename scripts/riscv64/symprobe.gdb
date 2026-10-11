# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# gdb helper: symbolize Gramine riscv64 crashes inside private (pal_rtld) mappings.
#
# libsysdb.so and the riscv libpal.so are mapped by Gramine's own pal_rtld, so gdb has no
# symbol files for them; libsysdb also ships no unwind info, so `bt` dies at frame 1. This
# probe loads the *build-tree* (unstripped, identical .text) copies at their runtime bases
# and prints a return-address-scanning pseudo-backtrace: every stack qword landing in an
# executable segment gets its nearest minimal symbol.
#
# Usage (in the guest, from the suite run dir):
#   gdb -batch -x scripts/riscv64/symprobe.gdb \
#       -ex 'set $sysdb="<build>/libos/src/libsysdb.so"' \
#       -ex 'set $pal="<build>/pal/src/host/linux/libpal.so"' \
#       --args <build>/prefix/lib/gramine/direct/loader \
#              <build>/prefix/lib/gramine/direct/libpal.so init <test>

set pagination off

python
import gdb, struct

norun = int(gdb.convenience_variable("norun") or 0)
if not norun:
    gdb.execute("handle SIGSYS nostop noprint pass")
    gdb.execute("run")

sysdb_path = gdb.convenience_variable("sysdb")
pal_path = gdb.convenience_variable("pal")

inf = gdb.selected_inferior()
maps = []
for line in gdb.execute("info proc mappings", to_string=True).splitlines():
    parts = line.split()
    if len(parts) < 4:
        continue
    try:
        start, end = int(parts[0], 16), int(parts[1], 16)
    except ValueError:
        continue
    name = parts[-1] if len(parts) >= 5 else ""
    maps.append((start, end, parts[2], name))

def load_symbols(objpath, match, textv):
    segs = [m for m in maps if m[3].endswith(match)]
    if not segs:
        print("NO MAPPING for %s" % match)
        return None
    base = min(m[0] for m in segs)
    # gdb's `add-symbol-file OBJ ADDR` places `.text` at ADDR (it prints `text_addr = ADDR`),
    # so ADDR must be the runtime address of .text = private-mapping base + .text vaddr.
    gdb.execute("add-symbol-file %s 0x%x" % (objpath, base + textv))
    exec_lo = min(m[0] for m in segs if "x" in m[2])
    exec_hi = max(m[1] for m in segs if "x" in m[2])
    print("%s: base=%s text=%s exec=[%s,%s)" % (match, hex(base), hex(base + textv),
          hex(exec_lo), hex(exec_hi)))
    return (exec_lo, exec_hi)

ranges = []
if sysdb_path:
    # .text vaddr of libsysdb (readelf -SW: 0xbf40); gdb has no solib entry for the
    # pal_rtld-private mapping, so we must supply it. libpal is gdb's exec file: its
    # automatically loaded symbols are already correct -- do NOT reload them here.
    textv = int(gdb.convenience_variable("sysdbtext") or 0)
    r = load_symbols(str(sysdb_path), "libsysdb.so", textv)
    ranges += [r] if r else []

print("\n=== fault frame ===")
print(gdb.execute("bt 6", to_string=True))
print(gdb.execute("x/4i $pc", to_string=True))

print("\n=== pseudo-backtrace (stack scan for return addresses) ===")
sp = int(gdb.parse_and_eval("$sp"))
mem = inf.read_memory(sp, 0x3000).tobytes()
seen = 0
for off in range(0, len(mem) - 7, 8):
    val = struct.unpack_from("<Q", mem, off)[0]
    for (lo, hi) in ranges:
        if lo <= val < hi:
            print("  sp+0x%04x  %s  %s" % (off, hex(val),
                  gdb.execute("info symbol 0x%x" % val, to_string=True).strip()))
            seen += 1
            break
    if seen > 24:
        break

def hexdump(addr, n, label):
    print("\n=== %s around %s ===" % (label, hex(addr)))
    try:
        mem = inf.read_memory(addr, n).tobytes()
    except gdb.error as e:
        print("  unreadable: %s" % e)
        return
    for off in range(0, n, 8):
        val = struct.unpack_from("<Q", mem, off)[0]
        raw = mem[off:off+8]
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in raw)
        tag = ""
        try:
            s = gdb.execute("info symbol 0x%x" % val, to_string=True).strip()
            if "No symbol" not in s:
                tag = "  <%s>" % s
        except gdb.error:
            pass
        print("  +%04x  %#018x  %-8s %s" % (off, val, asc, tag))

def cstr(addr, maxlen=120):
    try:
        raw = inf.read_memory(addr, maxlen).tobytes()
    except gdb.error:
        return None
    z = raw.find(b"\0")
    if z < 0:
        return None
    s = raw[:z]
    if len(s) < 3 or any(c < 32 or c > 126 for c in s):
        return None
    return s.decode()

for reg in ("$s2", "$s3"):
    try:
        v = int(gdb.parse_and_eval(reg))
    except gdb.error:
        continue
    if 0x1000 < v < 0x7fffffffffff:
        hexdump(v - 128, 256, "%s (qword view)" % reg)

# Scan the faulting stack for C strings (log buffers / format strings in flight).
print("\n=== strings on stack ===")
for off in range(0, len(mem) - 7, 8):
    val = struct.unpack_from("<Q", mem, off)[0]
    for (lo, hi) in ranges:
        if lo <= val < hi:
            s = cstr(val)
            if s:
                print("  sp+0x%04x  %s" % (off, s))
            break
end
