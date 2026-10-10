#!/bin/bash -e

# Attach to (or one-shot command inside) the aarch64 vm-PAL dev container
# created by ./build-vm-aarch64.sh. Counterpart of ./exec.sh for the arm64
# workflow; runnable repeatedly for extra shells.
#
#   ./exec-vm-aarch64.sh                  # interactive bash in the container
#   ./exec-vm-aarch64.sh '<command>'      # one-shot: bash -c '<command>'
#   ./exec-vm-aarch64.sh down             # stop and remove the container
CONTAINER=gramine-vm-aarch64

if [ "${1:-}" == "down" ]; then
    echo "Container being stopped"
    podman stop "$CONTAINER" 2>/dev/null || true
    podman rm "$CONTAINER" 2>/dev/null || true
    exit 0
fi

if ! podman ps --format "{{.Names}}" | grep -q "^${CONTAINER}$"; then
    if podman ps -a --format "{{.Names}}" | grep -q "^${CONTAINER}$"; then
        echo "Starting existing container..."
        podman start "$CONTAINER" >/dev/null
    else
        echo "Container doesn't exist; run ./build-vm-aarch64.sh first." >&2
        exit 1
    fi
fi

if [ $# -gt 0 ]; then
    # One-shot: pipes cleanly (no -t), so this composes with grep/redirects
    # exactly like the `podman run --rm ... -c '<runner command>'` invocations
    # the ARM64 milestone docs use.
    exec podman exec -i "$CONTAINER" bash -c "$*"
else
    exec podman exec -it "$CONTAINER" bash
fi
