#!/bin/bash -e

# Build the aarch64 vm-PAL dev image and (re)create its long-lived container.
# Counterpart of ./build.sh for the arm64 workflow; uses plain podman instead
# of podman-compose because the compose file mounts X11/Steam/GPU devices that
# do not exist on the headless aarch64 hosts this image targets.
#
# Host requirements: native aarch64 with working KVM (/dev/kvm) -- the runner
# defaults to `-accel kvm -cpu host` (TCG is out of scope, see the runner's
# header and Documentation/devel/aarch64/vm-pal-plan.md).
set -u

HERE=$(dirname "$(readlink -f "$0")")
IMAGE=gramine-vm-aarch64
CONTAINER=gramine-vm-aarch64
# The repo root is mounted where every documented command expects it.
REPO_ROOT=$(cd "$HERE/.." && pwd)

if [ ! -e /dev/kvm ]; then
    echo "warning: this host has no /dev/kvm; the container will start but" >&2
    echo "         run-vm-aarch64-dev.sh (ACCEL=kvm) cannot run here." >&2
fi

# Stop/remove any previous dev container, then rebuild the image (podman's
# layer cache makes rebuilds of an unchanged Containerfile near-instant).
podman stop "$CONTAINER" 2>/dev/null || true
podman rm "$CONTAINER" 2>/dev/null || true
podman build -f "$HERE/Containerfile.vm-aarch64" -t "$IMAGE" "$HERE"

# Long-lived container (exec-vm-aarch64.sh attaches shells to it, like
# `podman-compose up -d` + exec.sh do for the x86 flow).
#
# --cap-add ALL: virtiofsd serves the container root as the guest's / and the
#   vhost-user memfd mapping needs root-grade privileges; proven set for the
#   runner.
# --device /dev/kvm: mandatory, the target accelerator.
podman run -d --name "$CONTAINER" \
    --cap-add ALL \
    --device /dev/kvm:/dev/kvm \
    -v "$REPO_ROOT:/root/gramine-multiarch:rw" \
    "$IMAGE" sleep infinity

echo
echo "Container '$CONTAINER' is up (image $IMAGE, repo at /root/gramine-multiarch)."
echo "Attach a shell with ./exec-vm-aarch64.sh, or run one directly:"
echo "  ./exec-vm-aarch64.sh 'SMP=4 /root/gramine-multiarch/tools/run-vm-aarch64-dev.sh'"
