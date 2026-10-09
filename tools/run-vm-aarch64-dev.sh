#!/usr/bin/env bash
# Development bring-up runner for the aarch64 vm-PAL kernel (README-vm.md).
# Not the final launcher: tools/gramine-vm.in's arm counterpart will replace it.
#
# Usage: run-vm-aarch64-dev.sh [extra qemu args, e.g. "-accel kvm"]
set -e

REPO_ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
PAL=${PAL:-$REPO_ROOT/build-vm-aarch64/pal/src/host/vm/pal}
RUN_TIMEOUT=${RUN_TIMEOUT:-8} # override to tune boot window
WORK=$(mktemp -d /tmp/gramine-vm-aarch64.XXXXXX)
cleanup() {
    kill $(jobs -p) 2>/dev/null || true
    sleep 0.2
    # virtio-serial console is where PAL's log lands (PL011 only carries pre-driver
    # messages); dump it even when we're being terminated.
    if [ -s "$WORK/virtconsole.log" ]; then
        echo "=== virtconsole.log ===" >&2
        cat "$WORK/virtconsole.log" >&2
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

# QEMU's arm direct boot gives ELF -kernel images no DTB (it only synthesizes one
# for raw Image/uImage "linux" kernels and rejects -dtb blobs past the image base),
# so dump the machine DTB, repack it with dtc (drops the 1MB dumpdtb padding) and
# load it as a plain RAM blob at the DRAM base; pal_start_c() finds it there and
# authenticates the FDT magic.
M="-machine virt,gic-version=3,highmem-ecam=off"
qemu-system-aarch64 $M -accel tcg -cpu max -m 2G -smp 1 -nic none \
    -machine dumpdtb="$WORK/virt.dtb" >/dev/null
dtc -I dtb -O dtb -o "$WORK/virt-packed.dtb" "$WORK/virt.dtb"

# virtio-fs backend, mirroring tools/gramine-vm.in: virtiofsd serves / over a unix
# socket and QEMU's RAM must be a shared memfd backend for the vhost-user mapping.
# (Ubuntu's virtiofsd package installs it under /usr/lib/qemu, off PATH.)
VIRTIOFSD=$(command -v virtiofsd || echo /usr/lib/qemu/virtiofsd)
"$VIRTIOFSD" --socket-path "$WORK/vhostfs" --shared-dir / --log-level error \
    --sandbox none --no-announce-submounts >"$WORK/virtiofsd.log" 2>&1 &
for _ in $(seq 50); do
    [ -e "$WORK/vhostfs" ] && break
    sleep 0.1
done
if [ ! -e "$WORK/vhostfs" ]; then
    echo "virtiofsd failed to start:" >&2
    cat "$WORK/virtiofsd.log" >&2
    exit 1
fi

timeout ${RUN_TIMEOUT:-20} qemu-system-aarch64 $M "$@" -cpu max -m 2G -smp 1 -nic none \
    -object memory-backend-memfd,id=ram1,size=2G,share=on -machine memory-backend=ram1 \
    -fw_cfg name=opt/gramine/unixtime_s,string="$(date +%s)" \
    -fw_cfg name=opt/gramine/args,string="-gramine-args init /bin/hello -gramine-args-end" \
    -fw_cfg name=opt/gramine/envs,string="-gramine-envs -gramine-envs-end" \
    -fw_cfg name=opt/gramine/pwd,string="/" \
    -device virtio-serial-pci,iommu_platform=off,romfile= \
    -chardev file,path="$WORK/virtconsole.log",id=virtioconsole0 \
    -device virtconsole,chardev=virtioconsole0 \
    -chardev socket,path="$WORK/vhostfs",id=vhostfs \
    -device vhost-user-fs-pci,iommu_platform=off,queue-size=1024,chardev=vhostfs,tag=graminefs,romfile= \
    -kernel "$PAL" -device loader,file="$WORK/virt-packed.dtb",addr=0x40000000 \
    -nographic -monitor none &
QPid=$!

# Exits on its own under RUN_TIMEOUT (the trap above dumps the PAL console log).
wait $QPid || true
