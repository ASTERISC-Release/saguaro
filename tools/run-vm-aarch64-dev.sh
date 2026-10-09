#!/usr/bin/env bash
# Development bring-up runner for the aarch64 vm-PAL kernel (README-vm.md).
# Not the final launcher: tools/gramine-vm.in's arm counterpart will replace it.
#
# Usage: run-vm-aarch64-dev.sh [extra qemu args]
#
# Target is **KVM on a native aarch64 host** (QEMU virt + in-kernel vGIC/VHE); TCG is
# out of scope (see ARM64_MILESTONE_B.md): -cpu host is required (max is TCG-only) and
# the container must expose --device /dev/kvm. Override with ACCEL=tcg/CPU=max only as
# a debugging fallback, not a supported configuration.
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
# SMP=4 (say) runs the PSCI bring-up: the dumpdtb *and* the boot must use the same
# -smp, because the kernel now takes its CPU count from the DTB's cpu@N nodes.
M="-machine virt,gic-version=3,highmem-ecam=off"
SMP=${SMP:-1}
ACCEL=${ACCEL:-kvm}
CPU=${CPU:-host}
# The dumpdtb and boot runs must agree on everything that shapes the DTB, so they
# share $ACCEL/$CPU (the KVM machine describes its timer/vGIC topology differently
# from TCG; the kernel takes CPU count and PSCI conduit from this blob).
qemu-system-aarch64 $M -accel "$ACCEL" -cpu "$CPU" -m 2G -smp "$SMP" -nic none \
    -machine dumpdtb="$WORK/virt.dtb" >/dev/null
dtc -I dtb -O dtb -o "$WORK/virt-packed.dtb" "$WORK/virt.dtb"

# virtio-fs backend, mirroring tools/gramine-vm.in: virtiofsd serves / over a unix
# socket and QEMU's RAM must be a shared memfd backend for the vhost-user mapping.
# (Ubuntu's virtiofsd package installs it under /usr/lib/qemu, off PATH.)
VIRTIOFSD=$(command -v virtiofsd || echo /usr/lib/qemu/virtiofsd)

# Stage the hello deployment into the shared filesystem (the container's / == the
# guest's /): /bin/hello.manifest comes from tools/vm-aarch64-stage and /bin/hello
# is the static aarch64 binary built from the neighboring hello.c. The vm kernel
# reads argv as `init /bin/hello` (fw_cfg above), i.e. manifest path
# /bin/hello.manifest; LibOS then loads /bin/hello per the manifest's [libos]
# entrypoint and libsysdb.so via loader.entrypoint (referenced in place below).
STAGE="$REPO_ROOT/tools/vm-aarch64-stage"
if [ ! -x "$STAGE/hello" ]; then
    if command -v cc >/dev/null; then
        # -static-pie, not -static: a non-PIE static binary is linked at 0x400680, below the
        # arm-virt DRAM base (0x40000000) -- the kernel's boot identity map would "back" it with
        # the physical MMIO hole and every app instruction fetch aborts. ET_DYN makes LibOS
        # mmap it at an ASLR'd DRAM-backed address instead (what x86 gets for free because
        # x86 PCs have RAM under 0x400000).
        cc -static-pie -Os -o "$STAGE/hello" "$STAGE/hello.c"
    else
        echo "tools/vm-aarch64-stage/hello not built; run on a host with cc:" >&2
        echo "  cc -static-pie -Os -o $STAGE/hello $STAGE/hello.c" >&2
        exit 1
    fi
fi
install -Dm755 "$STAGE/hello" /bin/hello
LIBSYSDB=$(readlink -f "$REPO_ROOT/build-vm-aarch64/libos/src/libsysdb.so")
sed "s|@LIBSYSDB@|$LIBSYSDB|" "$STAGE/hello.manifest.template" > /bin/hello.manifest

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

timeout ${RUN_TIMEOUT:-20} qemu-system-aarch64 $M "$@" -cpu max -m 2G -smp "$SMP" -nic none \
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
