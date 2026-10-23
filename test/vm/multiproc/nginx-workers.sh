#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

# nginx with master process and 4 workers under saguaro-vm (experimental multiprocess)
set -u
. ~/.local/saguaro-vm/env.sh
cd "${REPO:-$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)}"/CI-Examples/nginx
cp install/conf/nginx-saguaro.conf /tmp/nginx-conf.bak
cp nginx.manifest /tmp/nginx-manifest.bak
sed -i '/^master_process off;$/d' install/conf/nginx-saguaro.conf
[ -n "${LOGLVL:-}" ] && sed -i "s/^log_level = .*/log_level = \"$LOGLVL\"/" nginx.manifest
sed -i 's/^enable_sigterm_injection = true$/enable_sigterm_injection = true\nexperimental__multiprocess = true/' nginx.manifest
SAGUARO_CPU_NUM=${CPUS:-2} SAGUARO_VM_INBOUND="8002:8002,8444:8444" setsid timeout -s KILL 120 saguaro-vm nginx > /tmp/nginx-mp.log 2>&1 &
pid=$!
ok=0
for i in $(seq 1 60); do
  if curl -sf --max-time 5 http://127.0.0.1:8002/random/10K.1.html -o /tmp/n1.html 2>/dev/null; then ok=1; break; fi
  sleep 0.5
done
good=0
if [ $ok = 1 ]; then
  for i in $(seq 1 50); do curl -sf --max-time 5 http://127.0.0.1:8002/random/10K.1.html -o /tmp/n1.html && cmp -s /tmp/n1.html install/html/random/10K.1.html && good=$((good+1)); done
  curl -sfk --max-time 5 https://127.0.0.1:8444/random/10K.1.html -o /tmp/n2.html && cmp -s /tmp/n2.html install/html/random/10K.1.html && echo "https ok"
fi
echo "http first=$ok, 50 sequential fetches identical: $good"
grep -c "VM child exited\|nginx: worker" /tmp/nginx-mp.log
kill -KILL -$pid 2>/dev/null; sleep 1
cp /tmp/nginx-conf.bak install/conf/nginx-saguaro.conf
cp /tmp/nginx-manifest.bak nginx.manifest
grep -iE "error|panic|emerg|alert" /tmp/nginx-mp.log | head -5
