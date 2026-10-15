# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# CPython inside a VBS enclave (phase 3e): the interpreter, its extension modules and its standard
# library are packed into the signed enclave image; this script is a packed file too.

import collections, datetime, json, math, os, re, socket, struct, sys, threading, time
import hashlib

print("Python", sys.version.split()[0], "running inside a VBS enclave, platform", sys.platform)
print("executable", sys.executable, "prefix", sys.prefix)

counts = collections.Counter(re.findall(r"\w+", "the quick brown fox jumps over the lazy dog the end"))
print("re + collections:", counts.most_common(2))
print("json:", json.dumps({"pi": round(math.pi, 6), "fact20": math.factorial(20)}, sort_keys=True))
print("sha256(enclave):", hashlib.sha256(b"enclave").hexdigest()[:16], "...")
print("struct:", struct.pack("<IH", 0xDEADBEEF, 7).hex())
print("datetime (host clock):", datetime.datetime.now(datetime.timezone.utc).isoformat()[:19])
print("listdir /usr/lib/python3.12 (first 5):", sorted(os.listdir("/usr/lib/python3.12"))[:5])

results = []
def work(n):
    results.append(sum(i * i for i in range(n)))
threads = [threading.Thread(target=work, args=(100000 * (k + 1),)) for k in range(3)]
t0 = time.perf_counter()
for t in threads:
    t.start()
for t in threads:
    t.join()
print("threads:", len(results), "results, sorted first", sorted(results)[0], "in",
      round((time.perf_counter() - t0) * 1000), "ms")

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]
cli = socket.create_connection(("127.0.0.1", port))
conn, _ = srv.accept()
cli.sendall(b"hello over host sockets")
print("socket echo:", conn.recv(100).decode())
for s in (cli, conn, srv):
    s.close()

print("python test: passed")
