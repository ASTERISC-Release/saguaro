#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Run `make VBS=1 check` of each application example, one at a time, with a timeout, and write a
# TSV (app, status, seconds, log). Run in WSL on the Windows ARM64 VBS machine, from a checkout.
# Usage: run-apps.sh OUTDIR [TIMEOUT_SECONDS] [APP...]
# Statuses: pass, fail (nonzero exit), timeout (124), skipped (no vbs-check.sh/Makefile support).
# Read-only apart from the builds into each example's own folder; does not touch other branches.
set -u
OUT=$(realpath -m "$1"); T=${2:-900}; shift 2 2>/dev/null || shift $#
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
APPS=("$@")
[ ${#APPS[@]} -gt 0 ] || APPS=(helloworld python redis nginx memcached lighttpd sqlite curl iperf rust busybox nodejs)
mkdir -p "$OUT"
# Windows-side load (WSL's own uptime restarts with the VM): boot time and CPU % over 2 s
winload() {
    powershell.exe -NoProfile -Command "\$o=Get-CimInstance Win32_OperatingSystem; \$c=(Get-Counter '\\Processor(_Total)\\% Processor Time' -SampleInterval 1 -MaxSamples 2).CounterSamples | % {[math]::Round(\$_.CookedValue,1)}; 'boot='+\$o.LastBootUpTime+' cpu%='+(\$c -join ',')+' freeMB='+[int](\$o.FreePhysicalMemory/1024)" 2>/dev/null </dev/null | tr -d '\r'
}
{ echo "# $(date -Is) host=$(hostname) uptime: $(uptime)"; } > "$OUT/header.txt"
printf 'app\tstatus\tseconds\tlog\n' > "$OUT/results.tsv"
for a in "${APPS[@]}"; do
    d="$ROOT/CI-Examples/$a"
    log="$OUT/$a.log"
    if [ ! -f "$d/Makefile" ] || ! grep -q VBS "$d/Makefile"; then
        printf '%s\tskipped\t0\t-\n' "$a" >> "$OUT/results.tsv"; continue
    fi
    s=$(date +%s)
    echo "$a before: $(winload)" >> "$OUT/load.txt"
    ( cd "$d" && timeout "$T" make VBS=1 && timeout "$T" make VBS=1 check ) > "$log" 2>&1 < /dev/null
    rc=$?
    e=$(( $(date +%s) - s ))
    case $rc in 0) st=pass;; 124) st=timeout;; *) st="fail($rc)";; esac
    echo "$a after:  $(winload)" >> "$OUT/load.txt"
    printf '%s\t%s\t%s\t%s\n' "$a" "$st" "$e" "$log" >> "$OUT/results.tsv"
    ( cd "$d" && make VBS=1 clean >/dev/null 2>&1 </dev/null || true )
    echo "$a $st ${e}s"
done
echo "uptime after: $(uptime)" >> "$OUT/header.txt"
