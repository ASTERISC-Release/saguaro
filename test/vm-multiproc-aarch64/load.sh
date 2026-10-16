#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# AArch64 VM process qualification, in this checkout's container after a --vm install.
# Usage: GRAMINE_PREFIX=... GRAMINE_CPU_NUM=1|2|4 test/vm-multiproc-aarch64/load.sh bash|nginx|redis
# Nginx and Redis must first be built in CI-Examples; host ports are checked before use.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
CPUS=${GRAMINE_CPU_NUM:-2}
WORK=${WORK:-$REPO/.mp-load-aarch64}
mkdir -p "$WORK"
. "$PREFIX/env.sh"
export GRAMINE_CPU_NUM=$CPUS

case ${1:-} in
    bash)
        cd "$WORK"
        mkdir -p tmp
        cp "$REPO/test/vm-multiproc/manifest.template" .
        gramine-manifest -Darch_libdir="/lib/$(gcc -dumpmachine)" -Dentry=/usr/bin/bash \
            -Dprobedir="$WORK" manifest.template bash.manifest
        cat > pipeline.sh <<'SH'
[ -d /probe/tmp ] && export TMPDIR=/probe/tmp
seq 1 200000 | cut -c1-3 | sort -n | uniq -c | sort -rn | head -3 | md5sum
SH
        expected=$(bash pipeline.sh)
        timeout -s KILL 300 gramine-vm bash /probe/pipeline.sh > bash-$CPUS.log 2>&1
        actual=$(grep -- ' -$' bash-$CPUS.log | tail -1)
        [ "$actual" = "$expected" ] || { echo "bash mismatch: $actual vs $expected"; exit 1; }
        echo "bash: 200,000 lines through seven processes matched native ($CPUS vCPUs)" ;;
    redis)
        [ -f "$REPO/CI-Examples/redis/redis-server.manifest" ] || {
            echo "build CI-Examples/redis first" >&2; exit 2;
        }
        GRAMINE_PREFIX=$PREFIX GRAMINE_CPU_NUM=$CPUS WORK=$WORK/redis-$CPUS \
            REDIS_HOST_PORT=${REDIS_HOST_PORT:-16379} RERUN=0 \
            "$REPO/test/vm-multiproc/check-probes.sh" redis ;;
    nginx)
        D=$REPO/CI-Examples/nginx
        [ -x "$D/install/sbin/nginx" ] && [ -f "$D/nginx.manifest" ] || {
            echo "build CI-Examples/nginx first" >&2; exit 2;
        }
        PORT=${NGINX_HOST_PORT:-18002}
        TLS_PORT=${NGINX_TLS_HOST_PORT:-18444}
        python3 - "$PORT" "$TLS_PORT" <<'PY'
import socket, sys
for port in map(int, sys.argv[1:]):
    with socket.socket() as s:
        s.bind(("127.0.0.1", port))
PY
        cp "$D/nginx.manifest" "$WORK/nginx.manifest.orig"
        rm -f "$WORK/nginx_args.orig"
        cp "$D/nginx_args" "$WORK/nginx_args.orig" 2>/dev/null || true
        cp "$D/install/conf/nginx-gramine.conf" "$WORK/nginx.conf.orig"
        server_pid=
        cleanup() {
            if [ -n "$server_pid" ]; then
                kill -KILL -- "-$server_pid" 2>/dev/null || true
                wait "$server_pid" 2>/dev/null || true
            fi
            cp "$WORK/nginx.manifest.orig" "$D/nginx.manifest"
            cp "$WORK/nginx.conf.orig" "$D/install/conf/nginx-gramine.conf"
            if [ -f "$WORK/nginx_args.orig" ]; then
                cp "$WORK/nginx_args.orig" "$D/nginx_args"
            else
                rm -f "$D/nginx_args"
            fi
        }
        trap cleanup EXIT
        sed -i '/^enable_sigterm_injection = true$/a experimental__multiprocess = true' \
            "$D/nginx.manifest"
        sed -i 's|^#error_log  logs/error.log  notice;|error_log  logs/error.log  notice;|' \
            "$D/install/conf/nginx-gramine.conf"
        gramine-argv-serializer nginx -p "$D/install/" -c conf/nginx-gramine.conf \
            > "$D/nginx_args"
        error_log=$D/install/logs/error.log
        before=0
        [ ! -f "$error_log" ] || before=$(stat -c %s "$error_log")
        (cd "$D" && GRAMINE_VM_INBOUND="8002:$PORT,8444:$TLS_PORT" \
            exec setsid timeout -s KILL 300 gramine-vm nginx \
            > "$WORK/nginx-$CPUS.log" 2>&1) &
        server_pid=$!
        python3 - "$PORT" "$D/install/html/random/10K.1.html" <<'PY'
import concurrent.futures, sys, time, urllib.request
port, path = int(sys.argv[1]), sys.argv[2]
url = f"http://127.0.0.1:{port}/random/10K.1.html"
expected = open(path, "rb").read()
for _ in range(60):
    try:
        if urllib.request.urlopen(url, timeout=2).read() == expected:
            break
    except Exception:
        time.sleep(0.5)
else:
    sys.exit("nginx: no matching response")
def fetch(_):
    try:
        return urllib.request.urlopen(url, timeout=10).read() == expected
    except Exception:
        return False
with concurrent.futures.ThreadPoolExecutor(8) as pool:
    good = sum(pool.map(fetch, range(200)))
print(f"nginx: {good}/200 identical responses with 8 clients")
if good != 200:
    sys.exit(1)
PY
        sleep 1
        workers=$(tail -c +$((before + 1)) "$error_log" 2>/dev/null \
            | grep -c 'start worker process ' || true)
        echo "nginx: $workers worker starts recorded ($CPUS vCPUs)"
        [ "$workers" -ge 4 ] ;;
    *) echo "usage: $0 bash|nginx|redis" >&2; exit 2 ;;
esac
