#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# The regression check of the VM graphics redirection's ioctl path from demand-paged processes
# (dev docs: proton-vm/design-gpu.md: the bounce buffers and the pointer table, option
# B; the shared session, option S): vk-headless alone, then N vk-headless processes at once
# through vk-two, under gramine-vm with the multiprocess key on. Needs the GPU (EGL_DISCRETE=1
# for renderD129, the AMD GPU; 0 for the first render node) and the QEMU fork with the listener.
# Usage (in the container, after scripts/install.sh --vm):
#   test/vulkan/vm-check.sh [N]        (default N=2; GRAMINE_PREFIX, QEMU_PATH, EGL_DISCRETE apply)
# The guest's Vulkan loader lists every GPU of the host and vk-headless picks device 0 unless
# VKH_DEVICE names one (an index or a name substring); on a machine with an AMD and an Intel GPU
#   VKH_DEVICE=Intel test/vulkan/vm-check.sh
# runs the checks on anv, that is through the i915 entries of the LibOS pointer table.
# Prints one PASS/FAIL line per run and exits non-zero when any failed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
N=${1:-2}
. "${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}/env.sh"
[ -n "${QEMU_PATH:-}" ] || { [ -x /root/qemu-tirith/build/qemu-system-x86_64 ] && export QEMU_PATH=/root/qemu-tirith/build/qemu-system-x86_64; }
export EGL_DISCRETE=${EGL_DISCRETE:-1}
cd "$HERE" || exit 2
make -s vk-headless vk-two gfx-fdpass vk-session-shim.so >/dev/null || exit 2
mkdir -p /tmp/vkshim # the manifests mount it as the guest's /tmp (the session shim's memfd names)
echo "# $(date -u +%Y-%m-%dT%H:%MZ) vm-check N=$N EGL_DISCRETE=$EGL_DISCRETE $(uptime | sed 's/.*load/load/')"
filter() { grep -v "preloader\|^(qemu)\|^$\|self-check\|Frame-pool\|boot\]\|REM_MEM\|vm-relay\|frame pool\|Configuring\|Physical\|memtype\|heap [0-9]"; }
rc=0
make -s vm-vk-headless.manifest MULTIPROCESS=1 >/dev/null 2>&1
out=$(timeout "${CHECK_TIMEOUT:-180}" gramine-vm vm-vk-headless 2>&1 | filter)
if echo "$out" | grep -q "^RESULT PASS"; then echo "PASS vm-vk-headless (one process): $(echo "$out" | grep -c '^STEP.*PASS') steps"
else echo "FAIL vm-vk-headless (one process)"; echo "$out" | tail -15; rc=1; fi
make -s vm-vk-two.manifest MULTIPROCESS=1 TWO=1 >/dev/null 2>&1
out=$(timeout "${CHECK_TIMEOUT:-240}" gramine-vm vm-vk-two "$N" 2>&1 | filter)
if echo "$out" | grep -q "^vk-two: $N of $N children ok"; then echo "PASS vm-vk-two ($N processes, one session): $(echo "$out" | grep -c '^RESULT PASS') results"
else echo "FAIL vm-vk-two ($N processes)"; echo "$out" | tail -25; rc=1; fi
make -s vm-gfx-fdpass.manifest MULTIPROCESS=1 EAGER=1 >/dev/null 2>&1
out=$(timeout "${CHECK_TIMEOUT:-180}" gramine-vm vm-gfx-fdpass 2>&1 | filter)
if echo "$out" | grep -q "^gfx-fdpass: RESULT PASS"; then echo "PASS vm-gfx-fdpass (a host fd over SCM_RIGHTS to a forked and to an exec'd process)"
else echo "FAIL vm-gfx-fdpass"; echo "$out" | tail -20; rc=1; fi
exit $rc
