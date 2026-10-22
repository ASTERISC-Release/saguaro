#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# guest.sh — the RISC-V Linux test guest of the riscv64 port (dev docs: riscv/PLAN.md).
#
# Runs an Ubuntu 24.04 riscv64 cloud image under qemu-system-riscv64 (full-system emulation, no
# KVM), with this repository shared into the guest at the same path it has here, so that paths
# baked into cross-built binaries and manifests are valid inside. Meant to be run in the
# cross-development container (`container/run.sh --target riscv64`).
#
# Usage:
#   scripts/riscv64/guest.sh start          # fetch/prepare on first use, boot, wait for ssh
#   scripts/riscv64/guest.sh ssh [command]  # shell, or run a command, in the guest (as root)
#   scripts/riscv64/guest.sh status
#   scripts/riscv64/guest.sh stop
#   scripts/riscv64/guest.sh reset          # stop and discard the guest's disk (keeps the download)
#
# Environment:
#   RISCV_GUEST_DIR    state directory (default: <repo>/.riscv64-guest, git-ignored; survives
#                      container recreation because the repository is a mount)
#   RISCV_GUEST_CPUS   vCPUs (default 4)        RISCV_GUEST_MEM   memory (default 4G)
#   RISCV_GUEST_PORT   host port forwarded to the guest's sshd (default 2222)
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DIR=${RISCV_GUEST_DIR:-$REPO/.riscv64-guest}
CPUS=${RISCV_GUEST_CPUS:-4}
MEM=${RISCV_GUEST_MEM:-4G}
PORT=${RISCV_GUEST_PORT:-2222}
IMAGE_URL=${RISCV_GUEST_IMAGE_URL:-https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-riscv64.img}
BASE=$DIR/base.img
DISK=$DIR/disk.qcow2
SEED=$DIR/seed.img
KEY=$DIR/id_ed25519
PIDFILE=$DIR/qemu.pid
CONSOLE=$DIR/console.log
SSH_OPTS=(-i "$KEY" -p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=10)

running() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

prepare() {
    mkdir -p "$DIR"
    if [ ! -f "$BASE" ]; then
        echo ">>> downloading $IMAGE_URL"
        wget -q --show-progress -O "$BASE.part" "$IMAGE_URL"
        mv "$BASE.part" "$BASE"
    fi
    [ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -C saguaro-riscv64-guest -f "$KEY"
    if [ ! -f "$DISK" ]; then
        qemu-img create -q -f qcow2 -F qcow2 -b "$BASE" "$DISK" 20G
        # First-boot configuration: root login with our key, the repository mounted at its usual
        # path, and the Python packages the Gramine tools and test suites import.
        cat > "$DIR/user-data" <<USERDATA
#cloud-config
disable_root: false
ssh_pwauth: false
users:
  - name: root
    ssh_authorized_keys:
      - $(cat "$KEY.pub")
mounts:
  - [ repo, $REPO, 9p, "trans=virtio,version=9p2000.L,msize=1048576,nofail,cache=none", "0", "0" ]
package_update: true
packages: [ python3-click, python3-jinja2, python3-pyelftools, python3-tomli, python3-tomli-w,
            python3-voluptuous, python3-pytest, python3-cryptography, python3-toml, file, strace, gdb,
            binutils, libgomp1 ]
runcmd:
  - mkdir -p $REPO
  - mount $REPO || true
  # large_mmap maps more than the guest's RAM; the default heuristic overcommit refuses that.
  - echo 'vm.overcommit_memory = 1' > /etc/sysctl.d/90-saguaro-tests.conf
  - sysctl -w vm.overcommit_memory=1
  - touch /var/lib/saguaro-guest-ready
USERDATA
        printf 'instance-id: saguaro-riscv64-%s\nlocal-hostname: riscv64-guest\n' "$(date +%s)" \
            > "$DIR/meta-data"
        cloud-localds "$SEED" "$DIR/user-data" "$DIR/meta-data"
    fi
}

start() {
    if running; then echo "guest already running (pid $(cat "$PIDFILE"))"; return 0; fi
    # Another checkout's guest (or any service) on the same port would receive our ssh commands.
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        echo "port $PORT is already in use (a guest of another checkout?); set RISCV_GUEST_PORT" >&2
        return 1
    fi
    prepare
    : > "$CONSOLE"
    qemu-system-riscv64 -machine virt -cpu rv64 -smp "$CPUS" -m "$MEM" \
        -bios /usr/lib/riscv64-linux-gnu/opensbi/generic/fw_jump.bin \
        -kernel /usr/lib/u-boot/qemu-riscv64_smode/uboot.elf \
        -drive "file=$DISK,format=qcow2,if=virtio" \
        -drive "file=$SEED,format=raw,if=virtio" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22" -device virtio-net-device,netdev=net0 \
        -fsdev "local,id=repo,path=$REPO,security_model=none" \
        -device virtio-9p-device,fsdev=repo,mount_tag=repo \
        -device virtio-rng-device \
        -display none -serial "file:$CONSOLE" -monitor none \
        -daemonize -pidfile "$PIDFILE"
    echo ">>> guest started (pid $(cat "$PIDFILE")); waiting for ssh on port $PORT (console: $CONSOLE)"
    local waited=0 limit=${RISCV_GUEST_BOOT_TIMEOUT:-1800}
    until ssh "${SSH_OPTS[@]}" root@127.0.0.1 'test -e /var/lib/saguaro-guest-ready' 2>/dev/null; do
        running || { echo "guest exited during boot; see $CONSOLE" >&2; tail -5 "$CONSOLE" >&2; return 1; }
        sleep 10; waited=$((waited + 10))
        [ "$waited" -lt "$limit" ] || { echo "guest not ready after ${limit}s; see $CONSOLE" >&2; return 1; }
    done
    echo ">>> guest ready after about ${waited}s"
}

case ${1:-} in
    start)  start ;;
    ssh)    shift; exec ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@" ;;
    status) if running; then echo "running (pid $(cat "$PIDFILE"))"; else echo "not running"; fi ;;
    stop)   if running; then
                ssh "${SSH_OPTS[@]}" root@127.0.0.1 poweroff 2>/dev/null || true
                for _ in $(seq 30); do running || break; sleep 2; done
                running && kill "$(cat "$PIDFILE")" 2>/dev/null || true
            fi
            rm -f "$PIDFILE"; echo "guest stopped" ;;
    reset)  "$0" stop >/dev/null; rm -f "$DISK" "$SEED" "$DIR/user-data" "$DIR/meta-data"
            echo "guest disk discarded (next start boots a fresh one)" ;;
    *)      sed -n '5,22p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
