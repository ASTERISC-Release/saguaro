#!/bin/bash
set -u
. ~/.local/saguaro-vm/env.sh
cd "${REPO:-$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)}"/CI-Examples/redis
cp redis-server.manifest /tmp/redis-manifest.bak
sed -i 's/^enable_sigterm_injection = true$/enable_sigterm_injection = true\nexperimental__multiprocess = true/' redis-server.manifest
rm -f dump.rdb
SAGUARO_CPU_NUM=${CPUS:-2} SAGUARO_VM_INBOUND="6379:6379" setsid timeout -s KILL 120 saguaro-vm redis-server > /tmp/redis-mp.log 2>&1 &
pid=$!
python3 - <<'PY'
import socket, time
def cmd(s, *a):
    s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    time.sleep(0.2); return s.recv(65536).decode(errors="replace")
for i in range(60):
    try:
        s = socket.create_connection(("127.0.0.1", 6379), timeout=5)
        if "PONG" in cmd(s, "PING"): break
    except Exception: time.sleep(0.5)
else:
    raise SystemExit("no PONG")
for i in range(1000): cmd(s, "SET", "k%d" % i, "v%d" % i) if i % 100 == 0 else s.sendall(b"")
print("BGSAVE ->", cmd(s, "BGSAVE").strip())
for i in range(40):
    info = cmd(s, "INFO", "persistence")
    if "rdb_bgsave_in_progress:0" in info and "rdb_last_bgsave_status" in info: break
    time.sleep(0.5)
print([l for l in info.splitlines() if l.startswith(("rdb_last_bgsave_status", "rdb_bgsave_in_progress", "rdb_changes_since_last_save"))])
print("GET k500 ->", cmd(s, "GET", "k500").strip().replace("\r\n", " "))
PY
sleep 1
kill -KILL -$pid 2>/dev/null; sleep 1
ls -l dump.rdb 2>&1
cp /tmp/redis-manifest.bak redis-server.manifest
grep -E "Background|DB saved|error|Error|panic" /tmp/redis-mp.log | head -8
rm -f dump.rdb
