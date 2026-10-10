#!/bin/bash -e

# Build the x86_64 dev image and (re)create its long-lived container. Counterpart of
# build-vm-aarch64.sh. Host requirements: x86_64 with /dev/kvm (vm PAL); the direct PAL needs no
# devices. /dev/vhost-vsock is passed through when present.
set -u

HERE=$(dirname "$(readlink -f "$0")")
IMAGE=gramine-vm-x86_64
CONTAINER=gramine-vm-x86_64
REPO_ROOT=$(cd "$HERE/.." && pwd)

if [ "$(uname -m)" != x86_64 ]; then
    echo "warning: this host is $(uname -m), not x86_64; the image will not be usable natively." >&2
fi
if [ ! -e /dev/kvm ]; then
    echo "warning: this host has no /dev/kvm; the container starts but gramine-vm cannot run." >&2
fi

podman stop "$CONTAINER" 2>/dev/null || true
podman rm "$CONTAINER" 2>/dev/null || true
podman build -f "$HERE/Containerfile.vm-x86_64" -t "$IMAGE" "$HERE"

DEVICES=()
for d in /dev/kvm /dev/vhost-vsock /dev/vhost-net /dev/net/tun; do
    [ -e "$d" ] && DEVICES+=(--device "$d:$d")
done

# --cap-add ALL / seccomp=unconfined: virtiofsd serves the container's / as the guest's /, and the
# direct PAL needs personality() (blocked by the default seccomp profile).
podman run -d --name "$CONTAINER" \
    --cap-add ALL \
    --security-opt seccomp=unconfined \
    "${DEVICES[@]}" \
    -v "$REPO_ROOT:/root/gramine-multiarch:rw" \
    "$IMAGE" sleep infinity

echo
echo "Container '$CONTAINER' is up (image $IMAGE, repo at /root/gramine-multiarch)."
echo "Attach with ./exec-vm-x86_64.sh, build with scripts/install.sh --vm (vm) or scripts/install.sh (direct)."
