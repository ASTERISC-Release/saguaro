#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# The network loads of dev docs: vbs-enclave/TESTING.md ("networking study"), from WSL on
# the Windows machine, against the examples built with `make VBS=1`: for each application of SUITE
# (redis nginx memcached lighttpd) the server is started in its enclave and each load is run with
# `gramine-vbs net bench`. With STATS=1 the host process counts its host calls
# (GRAMINE_VBS_HOSTCALL_STATS) into STATS_FILE (a Windows path; default
# %USERPROFILE%\gramine-vbs\hostcall-stats.txt), and the calls per request by kind are printed, with
# the calls an idle server makes outside the load subtracted. Results go to ~/vbsnet-logs/bench-TAG.
# Request counts: N1 (one connection), N8 (eight), NP (pipelined), NM (100 KiB), NL (1 MiB).
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
G=$R/scripts/vbs/gramine-vbs
OUTD=~/vbsnet-logs/bench-${TAG:-x}
mkdir -p $OUTD
WSTATS=${STATS_FILE:-$(powershell.exe -NoProfile -Command 'Write-Output $env:USERPROFILE' | tr -d '\r')\gramine-vbs\hostcall-stats.txt}
LSTATS=$(wslpath -u "$WSTATS")
snap() { sleep 0.3; local k=0; until cat $LSTATS > $1 2>/dev/null && [ -s $1 ]; do sleep 0.05; k=$((k+1)); [ $k -gt 100 ] && break; done; date +%s.%N > $1.t; }
diffstats() {  # A B requests bench-seconds -> per-kind counts per request (idle calls subtracted)
python3 - "$1" "$2" "$3" "$4" "$OUTD/idle0.txt" "$OUTD/idle1.txt" <<'PY'
import sys
def rd(p):
    d={}
    for l in open(p):
        f=l.split()
        if len(f)>=4: d[f[0]]=(int(f[1]),float(f[2]),float(f[3]),int(f[4]) if len(f)>4 else 0)
    return d
a,b,n=rd(sys.argv[1]),rd(sys.argv[2]),float(sys.argv[3])
secs=float(sys.argv[4])
def t(p): return float(open(p+'.t').read())
i0,i1=rd(sys.argv[5]),rd(sys.argv[6])
idle_span=t(sys.argv[6])-t(sys.argv[5])
extra=max(0.0,(t(sys.argv[2])-t(sys.argv[1]))-secs)   # time between the snapshots outside the bench
f=extra/idle_span if idle_span>0 else 0
for k in i1:
    z=i0.get(k,(0,0,0,0)); d=[i1[k][j]-z[j] for j in range(4)]
    if k in b: b[k]=tuple(b[k][j]-f*d[j] for j in range(4))
tot=0; out=[]
for k in sorted(b, key=lambda k:-(b[k][0]-a.get(k,(0,0,0,0))[0])):
    c=b[k][0]-a.get(k,(0,0,0,0))[0]
    if c/n<0.0005: continue
    h=b[k][1]-a.get(k,(0,0,0,0))[1]; e=b[k][2]-a.get(k,(0,0,0,0))[2]
    tot+=c
    by=b[k][3]-a.get(k,(0,0,0,0))[3]
    out.append(f"{k}={c/n:.3f} (host {h/c/1e3:.1f}us, encl {e/c/1e3:.1f}us" + (f", {by/c:.0f} B" if by else "") + ")")
print(f"  calls/request total {tot/n:.3f} (idle calls of {extra:.2f} s subtracted): "+"; ".join(out))
PY
}
run_load() {  # name port bench-args...
  local name=$1 port=$2; shift 2
  [ -n "${STATS:-}" ] && snap $OUTD/a.txt
  local line
  line=$($G net bench "$@" | tr -d '\r' | grep requests)
  echo "$name: $line" | tee -a $OUTD/results.txt
  if [ -n "${STATS:-}" ]; then
    snap $OUTD/b.txt
    local n secs; n=$(echo "$line" | sed -E 's/.*: ([0-9]+) requests.*/\1/')
    secs=$(echo "$line" | sed -E 's/.* ([0-9.]+) s, .*/\1/')
    diffstats $OUTD/a.txt $OUTD/b.txt $n $secs | tee -a $OUTD/results.txt
  fi
}
start() {  # dir app port
  cd $R/CI-Examples/$1
  rm -f $LSTATS
  if [ -n "${STATS:-}" ]; then export GRAMINE_VBS_HOSTCALL_STATS="$WSTATS"; else unset GRAMINE_VBS_HOSTCALL_STATS; fi
  $G $2 > $OUTD/$1-server.log 2>&1 &
  SPID=$!
  $G net wait $3 120 > /dev/null
  if [ -n "${STATS:-}" ]; then snap $OUTD/idle0.txt; sleep 3; snap $OUTD/idle1.txt; fi
}
stop() { kill $SPID 2>/dev/null; wait $SPID 2>/dev/null; sleep 1; }
for app in ${SUITE:-redis nginx memcached}; do
  case $app in
  redis)
    start redis redis-server 6379
    $G net bench redis-set 6379 -c 4 -n 20000 -k 10000 -w 0 > /dev/null
    run_load redis-get-c1 6379 redis-get 6379 -c 1 -n ${N1:-20000} -k 10000
    run_load redis-get-c8 6379 redis-get 6379 -c 8 -n ${N8:-100000} -k 10000
    run_load redis-get-c8-P16 6379 redis-get 6379 -c 8 -n ${NP:-800000} -k 10000 -P 16
    stop ;;
  nginx)
    start nginx nginx 8002
    run_load nginx-2K-c1 8002 http 8002 -c 1 -n ${N1:-20000} -p /random/2K.1.html
    run_load nginx-2K-c8 8002 http 8002 -c 8 -n ${N8:-100000} -p /random/2K.1.html
    run_load nginx-100K-c8 8002 http 8002 -c 8 -n ${NM:-10000} -p /random/100K.1.html
    run_load nginx-1M-c4 8002 http 8002 -c 4 -n ${NL:-1000} -p /random/1M.1.html
    run_load nginx-1M-c1 8002 http 8002 -c 1 -n ${NL:-1000} -p /random/1M.1.html
    stop ;;
  memcached)
    start memcached memcached 11211
    $G net bench memcached-set 11211 -c 4 -n 20000 -k 10000 -w 0 > /dev/null
    run_load memcached-get-c1 11211 memcached-get 11211 -c 1 -n ${N1:-20000} -k 10000
    run_load memcached-get-c8 11211 memcached-get 11211 -c 8 -n ${N8:-100000} -k 10000
    run_load memcached-get-c8-P16 11211 memcached-get 11211 -c 8 -n ${NP:-400000} -k 10000 -P 16
    stop ;;
  lighttpd)
    start lighttpd lighttpd 8003
    run_load lighttpd-2K-c8 8003 http 8003 -c 8 -n ${N8:-100000} -p /random/2K.1.html
    run_load lighttpd-100K-c8 8003 http 8003 -c 8 -n ${NM:-10000} -p /random/100K.1.html
    stop ;;
  esac
done
