#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# ltp-groups.sh -- the process LTP groups of Documentation/devel/vm-multiproc-aarch64/PLAN.md
# (vfork, execve family, fork, wait4 group, waitid, clone) under gramine-vm, one guest at a time,
# with the stock LTP install (.ltp-install, real fork) and its own harness directory, so that the
# manifests point at this prefix's LibOS. With --key on the manifests get
# sys.experimental__multiprocess (scripts/build-ltp-manifests.sh, LTP_VM_MULTIPROCESS=1).
#
# Usage: test/vm-multiproc-aarch64/ltp-groups.sh --prefix DIR --tag NAME [--key on|off]
#            [--rebuild] [--cpus N] [group...]
#   groups default: vfork execve fork wait4 waitid clone (names as in mapping.toml)
# Results: .ltp-results/<group>-<tag>-cpu<N>.json and one summary line per group.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PREFIX=; TAG=; KEY=off; REBUILD=0; CPUS=1; GROUPS_=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --key) KEY=$2; shift 2 ;;
        --rebuild) REBUILD=1; shift ;;
        --cpus) CPUS=$2; shift 2 ;;
        *) GROUPS_+=("$1"); shift ;;
    esac
done
[ -n "$PREFIX" ] && [ -n "$TAG" ] || { sed -n '2,16p' "$0"; exit 2; }
[ ${#GROUPS_[@]} -gt 0 ] || GROUPS_=(vfork execve fork wait4 waitid clone)
cd "$REPO"
HARNESS=$REPO/.ltp-harness-$TAG
mp=0; [ "$KEY" = on ] && mp=1
if [ $REBUILD = 1 ] || [ ! -d "$HARNESS" ]; then
    GRAMINE_PREFIX=$PREFIX LTP_PREFIX=$REPO/.ltp-install LTP_HARNESS_DIR=$HARNESS \
        LTP_VM_TMPFS_SHM=1 LTP_VM_MULTIPROCESS=$mp scripts/build-ltp-manifests.sh \
        > "$HARNESS.build.log" 2>&1 || { echo "manifest build failed: $HARNESS.build.log"; exit 1; }
fi
. "$PREFIX/env.sh"
mkdir -p .ltp-results
for g in "${GROUPS_[@]}"; do
    tests=$(python3 scripts/ltp-syscall.py "$g" --list)
    load=$(cut -d' ' -f1 /proc/loadavg)
    GRAMINE_CPU_NUM=$CPUS python3 scripts/run-ltp-syscalls.py --loader vm --ltp-root .ltp-install \
        --harness-dir "$HARNESS" --jobs 1 --timeout-factor 2 \
        --results ".ltp-results/$g-$TAG-cpu$CPUS.json" $tests > ".ltp-results/$g-$TAG-cpu$CPUS.log" 2>&1
    python3 - "$g" ".ltp-results/$g-$TAG-cpu$CPUS.json" "$load" <<'PY'
import json, sys
g, path, load = sys.argv[1:4]
r = json.load(open(path))["results"]
tot = {}
for t in r:
    tot[t["status"]] = tot.get(t["status"], 0) + 1
passed = sorted(t["test"] for t in r if t["status"] == "pass")
print(f"{g}: {tot.get('pass', 0)}/{len(r)} pass {dict(sorted(tot.items()))} load1={load} "
      f"passed={' '.join(passed)}")
PY
done
