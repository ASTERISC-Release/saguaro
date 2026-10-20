#!/usr/bin/env bash
# Boot-time and physical-commit probe for graphics VMs: boots glxgears under gramine-vm,
# samples the host's shmem/hugetlb commit while the guest renders, and prints the [boot]
# stamps (QEMU backend placement, PAL phases, guest library phases). Not a pass/fail
# test: its numbers feed the memory-reservation experiments in dev docs: graphics/.
#
# Usage: test/boot-probe.sh [RAM_SIZE]
#   GRAMINE_HUGEPAGES=1 GRAMINE_DATA_SIZE=3G test/boot-probe.sh 8G
set -euo pipefail

RAM=${1:-8G}
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO_ROOT=$(dirname "$HERE")
cd "$REPO_ROOT/CI-Examples/graphics/glxgears"

. ~/.local/gramine-vm/env.sh
export QEMU_PATH=${QEMU_PATH:-/root/qemu-tirith/build/qemu-system-x86_64}
export GRAMINE_RAM_SIZE=$RAM

meminfo() { grep -E "^(Shmem|HugePages_Total|HugePages_Free):" /proc/meminfo | tr '\n' ' '; echo; }
make 2>&1 | tail -1

echo "== before (RAM=$RAM hugepages=${GRAMINE_HUGEPAGES:-0}) =="
meminfo

LOG=$(mktemp)
GRAMINE_RAM_SIZE=$RAM timeout 45 make run >"$LOG" 2>&1 &
RUN_PID=$!
sleep 30   # guest is booted and rendering; physical commit has settled
echo "== during =="
meminfo
wait $RUN_PID || true
echo "== [boot] stamps =="
grep -E "Graphics data arena|Graphics arenas:|\[boot\]|\[mem\] host|FATAL|error:" "$LOG" | head -14
rm -f "$LOG"
echo "== after =="
meminfo