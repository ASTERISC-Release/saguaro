#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run.sh — open a shell in the development container for this machine's architecture.
#
# Detects the architecture (aarch64 or x86_64), builds the image if it is missing or its
# Containerfile changed, (re)creates the long-lived container if needed, starts it if stopped, and
# attaches a terminal. The repository is mounted at /root/gramine-multiarch.
#
# Usage:
#   container/run.sh                  # interactive shell
#   container/run.sh '<command>'      # one-shot: bash -c '<command>' (no tty, pipes cleanly)
#   container/run.sh --rebuild        # force an image rebuild and container recreation, then shell
#   container/run.sh down             # stop and remove the container (the image is kept)
#
# Environment: CONTAINER_NAME / IMAGE_NAME override the default gramine-vm-<arch> names.
#
# Needs a native host of the same architecture (no emulation) with /dev/kvm for gramine-vm. The
# container runs with --cap-add ALL (virtiofsd serves its / as the guest's /) and
# seccomp=unconfined (the direct PAL calls personality(), which the default profile blocks).
# Recreating the container discards what is stored in its /tmp (builds in the repository survive).
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO_ROOT=$(dirname "$HERE")

case $(uname -m) in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64)  ARCH=x86_64 ;;
    *) echo "unsupported architecture: $(uname -m) (need aarch64 or x86_64)" >&2; exit 1 ;;
esac
CONTAINERFILE=$HERE/Containerfile.vm-$ARCH
IMAGE=${IMAGE_NAME:-gramine-vm-$ARCH}
CONTAINER=${CONTAINER_NAME:-gramine-vm-$ARCH}
command -v podman >/dev/null || { echo "podman is required" >&2; exit 1; }
[ -f "$CONTAINERFILE" ] || { echo "missing $CONTAINERFILE" >&2; exit 1; }

REBUILD=0
if [ "${1:-}" = "--rebuild" ]; then REBUILD=1; shift; fi

if [ "${1:-}" = "down" ]; then
    podman stop "$CONTAINER" >/dev/null 2>&1 || true
    podman rm "$CONTAINER" >/dev/null 2>&1 || true
    echo "container $CONTAINER removed (image $IMAGE kept)"
    exit 0
fi

[ -e /dev/kvm ] || echo "warning: no /dev/kvm on this host; the container works but gramine-vm cannot run." >&2

# --- image: rebuild when missing, forced, or the Containerfile changed since the last build -------
WANT=$(sha256sum "$CONTAINERFILE" | cut -d' ' -f1)
HAVE=$(podman image inspect "$IMAGE" --format '{{index .Labels "gramine.containerfile-sha"}}' 2>/dev/null || true)
if [ "$REBUILD" = 1 ] || [ "$HAVE" != "$WANT" ]; then
    echo ">>> building image $IMAGE ($ARCH)"
    podman build -f "$CONTAINERFILE" --label "gramine.containerfile-sha=$WANT" -t "$IMAGE" "$HERE" >&2
fi
IMAGE_ID=$(podman image inspect "$IMAGE" --format '{{.Id}}')

# --- container: recreate when missing or created from an older image ------------------------------
if podman container exists "$CONTAINER"; then
    CUR_ID=$(podman container inspect "$CONTAINER" --format '{{.Image}}')
    if [ "$CUR_ID" != "$IMAGE_ID" ]; then
        echo ">>> container $CONTAINER is from an older image; recreating it"
        podman stop "$CONTAINER" >/dev/null 2>&1 || true
        podman rm "$CONTAINER" >/dev/null
    fi
fi
if ! podman container exists "$CONTAINER"; then
    DEVICES=()
    for d in /dev/kvm /dev/vhost-vsock /dev/vhost-net /dev/net/tun /dev/dri/renderD*; do
        [ -e "$d" ] && DEVICES+=(--device "$d:$d")
    done
    # Graphics (x86_64, see Documentation/devel/graphics/): the host X socket and DISPLAY for the
    # QEMU listener's window, and the QEMU fork when it is checked out next to this repository.
    GRAPHICS=()
    if [ -d /tmp/.X11-unix ] && [ -n "${DISPLAY:-}" ]; then
        GRAPHICS+=(-v /tmp/.X11-unix:/tmp/.X11-unix:ro -e "DISPLAY=$DISPLAY")
    fi
    QEMU_TIRITH=${QEMU_TIRITH_DIR:-$(dirname "$REPO_ROOT")/qemu-tirith}
    [ -d "$QEMU_TIRITH" ] && GRAPHICS+=(-v "$QEMU_TIRITH:/root/qemu-tirith:rw")
    echo ">>> creating container $CONTAINER"
    podman run -d --name "$CONTAINER" --cap-add ALL --security-opt seccomp=unconfined \
        ${DEVICES[@]+"${DEVICES[@]}"} ${GRAPHICS[@]+"${GRAPHICS[@]}"} \
        -v "$REPO_ROOT:/root/gramine-multiarch:rw" \
        "$IMAGE" sleep infinity >/dev/null
elif [ "$(podman container inspect "$CONTAINER" --format '{{.State.Running}}')" != true ]; then
    echo ">>> starting container $CONTAINER"
    podman start "$CONTAINER" >/dev/null
fi

# --- attach -----------------------------------------------------------------------------------------
if [ $# -gt 0 ]; then
    exec podman exec -i -w /root/gramine-multiarch "$CONTAINER" bash -c "$*"
else
    exec podman exec -it -w /root/gramine-multiarch "$CONTAINER" bash
fi
