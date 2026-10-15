#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Multi-process applications under load in gramine-vm (sys.experimental__multiprocess), run in the
# container after `scripts/install.sh --vm` and after the examples were built once (test-direct.sh):
#   nginx   master + 4 workers; N concurrent clients fetch a 10 KiB page, each body compared
#   redis   C clients write keys while BGSAVE forks a saving child R times; checks every save
#   bash    a pipeline of child processes over a large input, compared with the container's result
# Ports 8002/8444 (nginx) and 6379 (redis) must be free. CPUS sets the vCPUs (default 2).
# Usage: test/vm-multiproc/load.sh [nginx|redis|bash]...   (no argument: all three)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../.." && pwd)}
. ~/.local/gramine-vm/env.sh
CPUS=${CPUS:-2}
[ $# -gt 0 ] || set -- nginx redis bash

nginx_load() {
    cd "$REPO/CI-Examples/nginx"
    cp install/conf/nginx-gramine.conf /tmp/nginx-conf.bak
    cp nginx.manifest /tmp/nginx-manifest.bak
    sed -i '/^master_process off;$/d' install/conf/nginx-gramine.conf
    sed -i 's/^enable_sigterm_injection = true$/enable_sigterm_injection = true\nexperimental__multiprocess = true/' nginx.manifest
    GRAMINE_CPU_NUM=$CPUS GRAMINE_VM_INBOUND="8002:8002,8444:8444" setsid timeout -s KILL 300 \
        gramine-vm nginx > /tmp/nginx-load.log 2>&1 &
    local pid=$!
    python3 - "${REQUESTS:-2000}" "${CLIENTS:-16}" <<'PY'
import concurrent.futures, sys, time, urllib.request
total, clients = int(sys.argv[1]), int(sys.argv[2])
url = "http://127.0.0.1:8002/random/10K.1.html"
expected = open("install/html/random/10K.1.html", "rb").read()
for _ in range(120):
    try:
        urllib.request.urlopen(url, timeout=5).read(); break
    except Exception:
        time.sleep(0.5)
else:
    raise SystemExit("nginx: no answer")
def fetch(_):
    try:
        return urllib.request.urlopen(url, timeout=20).read() == expected
    except Exception:
        return False
start = time.time()
with concurrent.futures.ThreadPoolExecutor(clients) as pool:
    good = sum(pool.map(fetch, range(total)))
took = time.time() - start
print(f"nginx: {good}/{total} identical with {clients} concurrent clients in {took:.1f} s "
      f"({total / took:.0f} req/s)")
PY
    kill -KILL -$pid 2>/dev/null; sleep 1
    cp /tmp/nginx-conf.bak install/conf/nginx-gramine.conf
    cp /tmp/nginx-manifest.bak nginx.manifest
    grep -iE "panic|emerg|alert|internal memory fault" /tmp/nginx-load.log | head -5
}

redis_load() {
    cd "$REPO/CI-Examples/redis"
    cp redis-server.manifest /tmp/redis-manifest.bak
    sed -i 's/^enable_sigterm_injection = true$/enable_sigterm_injection = true\nexperimental__multiprocess = true/' redis-server.manifest
    rm -f dump.rdb
    GRAMINE_CPU_NUM=$CPUS GRAMINE_VM_INBOUND="6379:6379" setsid timeout -s KILL 300 \
        gramine-vm redis-server > /tmp/redis-load.log 2>&1 &
    local pid=$!
    python3 - "${CLIENTS:-4}" "${KEYS:-5000}" "${SAVES:-5}" <<'PY'
import socket, sys, threading, time
clients, keys, saves = (int(a) for a in sys.argv[1:4])
def conn():
    # the port relay accepts before redis listens: retry until PING answers
    for _ in range(120):
        try:
            s = socket.create_connection(("127.0.0.1", 6379), timeout=30)
            if "PONG" in call(s, "PING"):
                return s
        except (OSError, SystemExit):
            pass
        time.sleep(0.5)
    raise SystemExit("redis: no answer")
def call(s, *a):
    s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    data = b""
    while not data.endswith(b"\r\n"):
        chunk = s.recv(65536)
        if not chunk:
            raise SystemExit("redis: connection closed")
        data += chunk
    if data.startswith(b"$") and not data.startswith(b"$-1"):
        n = int(data[1:data.index(b"\r\n")])
        while len(data) < data.index(b"\r\n") + 2 + n + 2:
            data += s.recv(65536)
    return data.decode(errors="replace")
def writer(c):
    s = conn()
    for i in range(keys):
        assert call(s, "SET", f"c{c}:k{i}", f"v{i}").startswith("+OK")
ctl = conn()
threads = [threading.Thread(target=writer, args=(c,)) for c in range(clients)]
for t in threads: t.start()
ok = 0
for n in range(saves):
    time.sleep(0.5)
    while "Background saving started" not in call(ctl, "BGSAVE"):
        time.sleep(0.2)
    for _ in range(240):
        info = call(ctl, "INFO", "persistence")
        if "rdb_bgsave_in_progress:0" in info:
            break
        time.sleep(0.25)
    ok += "rdb_last_bgsave_status:ok" in info
for t in threads: t.join()
size = call(ctl, "DBSIZE").strip()
value = call(ctl, "GET", f"c{clients - 1}:k{keys - 1}").split("\r\n")[1]
print(f"redis: {ok}/{saves} BGSAVE ok while {clients} clients wrote {keys} keys each; "
      f"DBSIZE {size}, last key {value}")
PY
    kill -KILL -$pid 2>/dev/null; sleep 1
    cp /tmp/redis-manifest.bak redis-server.manifest
    echo "redis: saving children: $(grep -c 'DB saved on disk' /tmp/redis-load.log)"
    grep -iE "panic|internal memory fault|error" /tmp/redis-load.log | head -5
    rm -f dump.rdb
}

bash_load() {
    local work=${WORK:-$REPO/.mp-probes}
    mkdir -p "$work" && cd "$work"
    cp "$HERE"/manifest.template .
    gramine-manifest -Darch_libdir=/lib/x86_64-linux-gnu -Dentry=/usr/bin/bash -Dprobedir="$work" \
        manifest.template bash.manifest 2>/dev/null
    # a script file, so that the pipeline needs no quoting through the gramine-vm command line
    mkdir -p tmp
    cat > pipeline.sh <<'SH'
[ -d /probe/tmp ] && export TMPDIR=/probe/tmp   # in the guest: the probe manifest has no /tmp
seq 1 200000 | awk '{print $1 % 977}' | sort -n | uniq -c | sort -rn | head -3 | md5sum
SH
    local want got
    want=$(bash pipeline.sh)
    got=$(GRAMINE_CPU_NUM=$CPUS timeout -s KILL 300 gramine-vm bash /probe/pipeline.sh 2>/dev/null | grep -- ' -$')
    [ "$got" = "$want" ] && echo "bash: pipeline of 6 processes over 200,000 lines: identical" \
                         || echo "bash: pipeline output differs: '$got' vs '$want'"
}

for what in "$@"; do
    case $what in
        nginx) nginx_load ;;
        redis) redis_load ;;
        bash)  bash_load ;;
        *) echo "unknown: $what" >&2; exit 2 ;;
    esac
done
