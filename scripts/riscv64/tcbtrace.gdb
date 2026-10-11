# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# gdb helper: log every pal_thread_init invocation (the child-side entry whose *first* action
# writes the default stack-protector canary to the TCB pointer passed through the clone asm
# stub). Prints the argument (TCB pointer), the child's initial sp, and the TCB's first words,
# then dumps registers/backtrace/stack window when the run stops on a fault. Used to rule thread
# creation in or out as the writer of the "{TCB pointer, 0xbadbadbadbad}" trample pattern seen in
# the eventfd cluster (result: all invocations receive valid TCB pointers).
python
import gdb
gdb.execute("set pagination off")
gdb.execute("set env PAD = " + "A" * 60000)

class PTI(gdb.Breakpoint):
    hits = 0
    def __init__(self):
        super().__init__("pal_thread_init")
    def stop(self):
        PTI.hits += 1
        a0 = int(gdb.parse_and_eval("$a0"))
        sp = int(gdb.parse_and_eval("$sp"))
        words = []
        try:
            raw = gdb.selected_inferior().read_memory(a0, 40).tobytes()
            words = [int.from_bytes(raw[i:i+8], "little") for i in range(0, 40, 8)]
        except gdb.error:
            words = None
        print("PTI %d: tcb=0x%x sp=0x%x head=%s" %
              (PTI.hits, a0, sp, [hex(w) for w in words] if words else "unreadable"))
        return False

PTI()
try:
    gdb.execute("run")
except gdb.error as e:
    print("run stopped: %s" % e)
try:
    inf = gdb.selected_inferior()
    pc = int(gdb.parse_and_eval("$pc"))
    sp = int(gdb.parse_and_eval("$sp"))
    a0 = int(gdb.parse_and_eval("$a0"))
    ra = int(gdb.parse_and_eval("$ra"))
    print("pc=0x%x sp=0x%x ra=0x%x a0=0x%x" % (pc, sp, ra, a0))
    print(gdb.execute("info symbol 0x%x" % pc, to_string=True).strip())
    print(gdb.execute("info symbol 0x%x" % ra, to_string=True).strip())
    gdb.execute("bt 12")
    raw = inf.read_memory(sp, 0x100).tobytes()
    import struct
    for i in range(0, 0x100, 8):
        v = struct.unpack_from("<Q", raw, i)[0]
        print("  sp+%02x: 0x%016x" % (i, v))
except gdb.error as e:
    print("dump failed: %s" % e)
print("pal_thread_init total hits: %d" % PTI.hits)
end
