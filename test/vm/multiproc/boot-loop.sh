#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Starts N gramine-vm guests back to back (as a CI step or the LibOS suite does) and counts the ones
# that do not print the program's output or exit status: a guest that dies early in boot prints only
# the launcher lines ("Configuring Gramine VM ...", "vm-relay: ... listening ...") and, since the
# launcher checks for that, "Error: the guest ended without reporting an exit code" with the guest's
# serial output. Run in the container after `scripts/install.sh --vm`. LOAD=32 at 2 vCPUs is what
# reproduced the boot crash of Documentation/devel/vm-multiproc/FINDINGS.md (2026-10-15).
#
# Usage: test/vm-multiproc/boot-loop.sh [N]   (default 200)
# Environment: GRAMINE_CPU_NUM (default 2), LOAD=<k> runs k busy loops on the host meanwhile,
# GAP=<seconds> sleeps between guests, WORK (default <repo>/.mp-probes/boot-loop), GRAMINE_PREFIX.
# Each failing run's output goes to $WORK/fail-<i>.log. Exit status 0 only if every guest printed.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
. "$PREFIX/env.sh"
export GRAMINE_CPU_NUM=${GRAMINE_CPU_NUM:-2}
N=${1:-200}
WORK=${WORK:-$REPO/.mp-probes/boot-loop}
mkdir -p "$WORK" && cd "$WORK" && rm -f fail-*.log
cat > hello.c <<'EOF'
#include <stdio.h>
int main(void) { printf("boot-loop: hello\n"); return 7; }
EOF
gcc -O2 -o hello hello.c || exit 2
cat > hello.manifest.template <<'EOF'
libos.entrypoint = "/probe/hello"
loader.log_level = "error"
loader.env.LD_LIBRARY_PATH = "/lib:{{ arch_libdir }}"
fs.mounts = [
  { path = "/lib", uri = "file:{{ gramine.runtimedir() }}" },
  { path = "{{ arch_libdir }}", uri = "file:{{ arch_libdir }}" },
  { path = "/probe", uri = "file:{{ probedir }}" },
]
EOF
gramine-manifest -Darch_libdir=/lib/$(gcc -dumpmachine) -Dprobedir="$WORK" hello.manifest.template \
    hello.manifest 2>/dev/null || exit 2

LOAD_PIDS=()
for _ in $(seq 1 "${LOAD:-0}"); do ( while :; do :; done ) & LOAD_PIDS+=($!); done
trap '[ ${#LOAD_PIDS[@]} = 0 ] || kill "${LOAD_PIDS[@]}" 2>/dev/null' EXIT

fails=0
start=$(date +%s)
for i in $(seq 1 "$N"); do
    out=$(timeout -s KILL 60 gramine-vm hello 2>&1)
    rc=$?
    if ! grep -q "boot-loop: hello" <<<"$out" || [ "$rc" != 7 ]; then
        fails=$((fails + 1))
        printf '%s\n+ launcher exit status %s\n' "$out" "$rc" > "fail-$i.log"
        echo "run $i: FAILED (exit $rc; $WORK/fail-$i.log)"
    fi
    [ -n "${GAP:-}" ] && sleep "$GAP"
done
echo "boot-loop: $fails of $N guests failed ($GRAMINE_CPU_NUM vCPUs, load ${LOAD:-0}," \
     "gap ${GAP:-0}s, $(( $(date +%s) - start )) s)"
[ "$fails" = 0 ]
