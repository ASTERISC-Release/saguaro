#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run.sh — open a shell in the development container for this machine's architecture.
#
# Detects the architecture (aarch64 or x86_64), builds the image if it is missing or its
# Containerfile changed, (re)creates the long-lived container if needed, starts it if stopped, and
# attaches a terminal. The repository is mounted at /root/<name of the checkout's folder> (for example
# /root/saguaro).
#
# Usage:
#   container/run.sh                  # interactive shell
#   container/run.sh '<command>'      # one-shot: bash -c '<command>' (no tty, pipes cleanly)
#   container/run.sh --rebuild        # force an image rebuild and container recreation, then shell
#   container/run.sh down             # stop and remove the container (the image is kept)
#   container/run.sh --target riscv64 [...]   # the RISC-V cross-development container instead
#
# Environment: CONTAINER_NAME / IMAGE_NAME override the default names (container saguaro-vm-<arch>,
# image saguaro-vm-<arch>:cf-<Containerfile hash>; see the image section below).
# SAGUARO_CONTAINER_NVIDIA=1 (when the container is created) also passes the NVIDIA proprietary
# driver's devices and the host's NVIDIA userspace libraries (off by default).
#
# Workspaces (see Documentation/AGENTS.md): when the checkout sits in a folder named saguaro-<name> (for example
# .../saguaro-<name>/saguaro), the container is named after that folder
# (saguaro-<name>-vm-x86_64, saguaro-<name>-riscv64-cross), and gets its own base for the guest CIDs
# (SAGUARO_VM_ID_BASE), so several checkouts can run side by side. A folder named gramine-<name> (the
# earlier convention) is accepted too; the container is then named after it (gramine-<name>-vm-x86_64).
# SAGUARO_WORKSPACE sets the name by hand. Workspaces with the same Containerfile share the image. A
# container that does not mount this checkout at its mount point is refused instead of being
# silently reused.
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
# `--target riscv64` (first argument) selects the cross-development container of the RISC-V port
# instead of the native one: an x86_64 image with the riscv64 cross toolchain and
# qemu-system-riscv64 (dev docs: riscv/PLAN.md).
TARGET=
if [ "${1:-}" = "--target" ]; then TARGET=${2:-}; shift 2; fi
case $TARGET in
    "")
        CONTAINERFILE=$HERE/Containerfile.vm-$ARCH
        NAME=saguaro-vm-$ARCH ;;
    riscv64)
        [ "$ARCH" = x86_64 ] || { echo "the riscv64 cross container needs an x86_64 host" >&2; exit 1; }
        CONTAINERFILE=$HERE/Containerfile.riscv64-cross
        NAME=saguaro-riscv64-cross ;;
    *) echo "unknown --target '$TARGET' (known: riscv64)" >&2; exit 1 ;;
esac
WORKSPACE=${SAGUARO_WORKSPACE:-}
if [ -z "$WORKSPACE" ]; then
    case $(basename "$(dirname "$REPO_ROOT")") in
        saguaro-*|gramine-*) WORKSPACE=$(basename "$(dirname "$REPO_ROOT")") ;;
    esac
fi
DEFAULT_CONTAINER=$NAME
VM_ID_BASE=
if [ -n "$WORKSPACE" ]; then
    DEFAULT_CONTAINER=$WORKSPACE-${NAME#saguaro-}
    # 110, 210, ..., 4010: a base per workspace (the launcher only sees the QEMU processes of its
    # own container when it looks for a free guest CID, and CIDs are global on the host)
    VM_ID_BASE_STEP=$(( $(printf %s "$WORKSPACE" | cksum | cut -d' ' -f1) % 40 ))
    [ "$VM_ID_BASE_STEP" -ne 0 ] || VM_ID_BASE_STEP=40   # base 10 is the default outside workspaces
    VM_ID_BASE=$((10 + 100 * VM_ID_BASE_STEP))
fi
IMAGE=${IMAGE_NAME:-$NAME}
# the checkout is mounted under the name of its own folder, so that paths inside and outside the container
# end the same way
MOUNT_POINT=/root/$(basename "$REPO_ROOT")
CONTAINER=${CONTAINER_NAME:-$DEFAULT_CONTAINER}
command -v podman >/dev/null || { echo "podman is required" >&2; exit 1; }
[ -f "$CONTAINERFILE" ] || { echo "missing $CONTAINERFILE" >&2; exit 1; }

REBUILD=0
if [ "${1:-}" = "--rebuild" ]; then REBUILD=1; shift; fi

if [ "${1:-}" = "down" ]; then
    podman stop "$CONTAINER" >/dev/null 2>&1 || true
    podman rm "$CONTAINER" >/dev/null 2>&1 || true
    echo "container $CONTAINER removed (images kept)"
    exit 0
fi

[ -e /dev/kvm ] || echo "warning: no /dev/kvm on this host; the container works but gramine-vm cannot run." >&2

# --- image: one per Containerfile version ------------------------------------------------------------
# The image is tagged <name>:cf-<first 12 hex digits of the Containerfile's sha256>, so checkouts with
# different Containerfiles (branches) get different images and never rebuild or retag each other's: a
# container is recreated only when its own checkout's Containerfile changes. An image built earlier
# with the same Containerfile (it carries the sha in the label saguaro.containerfile-sha, also when it
# was built under the old single tag <name>) is reused by tagging it, without a build. IMAGE_NAME
# keeps the old behaviour: that one tag, rebuilt when the Containerfile differs from its label.
WANT=$(sha256sum "$CONTAINERFILE" | cut -d' ' -f1)
if [ -n "${IMAGE_NAME:-}" ]; then
    HAVE=$(podman image inspect "$IMAGE" --format '{{index .Labels "saguaro.containerfile-sha"}}' 2>/dev/null || true)
    if [ "$REBUILD" = 1 ] || [ "$HAVE" != "$WANT" ]; then
        echo ">>> building image $IMAGE ($ARCH)"
        podman build -f "$CONTAINERFILE" --label "saguaro.containerfile-sha=$WANT" -t "$IMAGE" "$HERE" >&2
    fi
else
    IMAGE=$NAME:cf-${WANT:0:12}
    if [ "$REBUILD" = 1 ] || ! podman image exists "$IMAGE"; then
        EXISTING=
        [ "$REBUILD" = 1 ] || EXISTING=$(podman images -q --filter "label=saguaro.containerfile-sha=$WANT" | head -n 1)
        if [ -n "$EXISTING" ]; then
            podman tag "$EXISTING" "$IMAGE"
        else
            echo ">>> building image $IMAGE ($ARCH)"
            podman build -f "$CONTAINERFILE" --label "saguaro.containerfile-sha=$WANT" -t "$IMAGE" "$HERE" >&2
        fi
    fi
fi
IMAGE_ID=$(podman image inspect "$IMAGE" --format '{{.Id}}')

# --- container: recreate when missing or created from an older image ------------------------------
if podman container exists "$CONTAINER"; then
    MOUNT_SRC=$(podman container inspect "$CONTAINER" \
                --format "{{range .Mounts}}{{if eq .Destination \"$MOUNT_POINT\"}}{{.Source}}{{end}}{{end}}")
    if [ -z "$MOUNT_SRC" ] || [ "$(readlink -f "$MOUNT_SRC")" != "$(readlink -f "$REPO_ROOT")" ]; then
        MOUNTS=$(podman container inspect "$CONTAINER" \
                 --format '{{range .Mounts}}{{if eq .Type "bind"}} {{.Source}}:{{.Destination}}{{end}}{{end}}')
        echo "error: container $CONTAINER does not mount this checkout ($REPO_ROOT) at $MOUNT_POINT" >&2
        echo "       (its bind mounts:${MOUNTS:- none})." >&2
        echo "       Use a container of its own (CONTAINER_NAME=..., or put the checkout in a saguaro-* folder)," >&2
        echo "       or run '$0 down' first. Removing a container discards files kept in it outside the repository" >&2
        echo "       (for example an install prefix under ~/.local)." >&2
        exit 1
    fi
    CUR_ID=$(podman container inspect "$CONTAINER" --format '{{.Image}}')
    if [ "$CUR_ID" != "$IMAGE_ID" ]; then
        echo ">>> container $CONTAINER is from another image than this checkout's Containerfile ($IMAGE); recreating it" >&2
        echo "    (files kept in the container outside the repository, such as an install prefix, are discarded)" >&2
        podman stop "$CONTAINER" >/dev/null 2>&1 || true
        podman rm "$CONTAINER" >/dev/null
    fi
fi
if ! podman container exists "$CONTAINER"; then
    DEVICES=()
    for d in /dev/kvm /dev/vhost-vsock /dev/vhost-net /dev/net/tun /dev/dri/renderD*; do
        [ -e "$d" ] && DEVICES+=(--device "$d:$d")
    done
    # AMD SEV (dev docs: sev/): QEMU opens /dev/sev for the launch commands of an SEV,
    # SEV-ES or SEV-SNP guest. The node is usually root-only; where the host gives it to a group
    # (e.g. `sev`, mode 0660), keep-groups carries the user's supplementary groups into the
    # rootless container so the device stays accessible (needs the crun runtime).
    if [ -e /dev/sev ]; then
        DEVICES+=(--device /dev/sev:/dev/sev)
        [ -r /dev/sev ] && [ -w /dev/sev ] && DEVICES+=(--group-add keep-groups)
    fi
    # Intel SGX: the enclave device of the in-kernel driver, and /dev/sgx_provision (quotes). The nodes belong
    # to the group `sgx` (mode 0660, provision usually root-only), so keep-groups carries the user's
    # supplementary groups into the rootless container (needs the crun runtime), as for /dev/sev.
    # They are bind-mounted with `exec`: Gramine maps /dev/sgx_enclave with PROT_EXEC, which fails with EPERM
    # on the `noexec` mount that `--device` makes in a rootless container (the loader's message then blames
    # vm.mmap_min_addr, which is not the cause).
    for d in /dev/sgx_enclave /dev/sgx_provision; do
        [ -e "$d" ] && DEVICES+=(-v "$d:$d:dev,exec")
    done
    if [ -r /dev/sgx_enclave ] && [ -w /dev/sgx_enclave ] && [[ " ${DEVICES[*]} " != *" keep-groups "* ]]; then
        DEVICES+=(--group-add keep-groups)
    fi
    # Graphics (x86_64, see dev docs: graphics/): the host X socket and DISPLAY for the
    # QEMU listener's window, and the QEMU fork when it is checked out next to this repository.
    GRAPHICS=()
    if [ -d /tmp/.X11-unix ] && [ -n "${DISPLAY:-}" ]; then
        GRAPHICS+=(-v /tmp/.X11-unix:/tmp/.X11-unix:ro -e "DISPLAY=$DISPLAY")
    fi
    QEMU_TIRITH=${QEMU_TIRITH_DIR:-$(dirname "$REPO_ROOT")/qemu-tirith}
    [ -d "$QEMU_TIRITH" ] && GRAPHICS+=(-v "$QEMU_TIRITH:/root/qemu-tirith:rw")
    # AMD SEV tooling (QEMU with SEV-SNP/IGVM, snpguest, ...; container/sev/build-sev-tools.sh):
    # kept next to this repository as sev-tools/ (or at SEV_TOOLS_DIR) so it survives recreation.
    SEV_TOOLS=${SEV_TOOLS_DIR:-$(dirname "$REPO_ROOT")/sev-tools}
    [ -d "$SEV_TOOLS" ] && GRAPHICS+=(-v "$SEV_TOOLS:/root/sev-tools:rw")
    # Intel SGX: the socket of the host's AESM service (remote attestation), when it runs
    [ -S /var/run/aesmd/aesm.socket ] && GRAPHICS+=(-v /var/run/aesmd:/var/run/aesmd)
    # Opt-in (SAGUARO_CONTAINER_NVIDIA=1, see dev docs: graphics-aarch64/): the NVIDIA
    # proprietary driver's device nodes and the host's matching userspace (GLX/EGL vendor libraries,
    # libnvidia-*, GBM backend, vendor configuration files), read-only. The libraries must match the
    # host's kernel module, so they are taken from the host instead of being installed in the image.
    if [ "${SAGUARO_CONTAINER_NVIDIA:-0}" = 1 ]; then
        for d in /dev/nvidia[0-9]* /dev/nvidiactl /dev/nvidia-modeset /dev/nvidia-uvm \
                 /dev/nvidia-uvm-tools /dev/dri/card*; do
            [ -e "$d" ] && DEVICES+=(--device "$d:$d")
        done
        LIBDIR=/usr/lib/$(uname -m)-linux-gnu
        for f in "$LIBDIR"/lib{GLX,EGL,GLESv1_CM,GLESv2}_nvidia.so* "$LIBDIR"/libnvidia-*.so* \
                 "$LIBDIR"/libcuda.so* "$LIBDIR"/gbm/nvidia-drm_gbm.so \
                 /usr/share/glvnd/egl_vendor.d/*nvidia*.json \
                 /usr/share/egl/egl_external_platform.d/*nvidia*.json \
                 /usr/share/vulkan/icd.d/nvidia_icd.json /usr/share/nvidia; do
            case $f in *libnvidia-container*) continue ;; esac
            [ -e "$f" ] && GRAPHICS+=(-v "$f:$f:ro")
        done
    fi
    echo ">>> creating container $CONTAINER"
    # --init: a minimal init as PID 1 reaps orphaned helper processes (e.g. of gramine-vm) when the
    # host has one (catatonit); without it they stay around as zombies until the container stops.
    INIT=()
    command -v catatonit >/dev/null 2>&1 && INIT=(--init)
    podman run -d --name "$CONTAINER" ${INIT[@]+"${INIT[@]}"} --cap-add ALL --security-opt seccomp=unconfined \
        ${DEVICES[@]+"${DEVICES[@]}"} ${GRAPHICS[@]+"${GRAPHICS[@]}"} \
        ${VM_ID_BASE:+-e "SAGUARO_VM_ID_BASE=$VM_ID_BASE"} \
        -v "$REPO_ROOT:$MOUNT_POINT:rw" \
        "$IMAGE" sleep infinity >/dev/null
elif [ "$(podman container inspect "$CONTAINER" --format '{{.State.Running}}')" != true ]; then
    echo ">>> starting container $CONTAINER"
    podman start "$CONTAINER" >/dev/null
fi

# --- attach -----------------------------------------------------------------------------------------
# Both modes load the Saguaro environment of the vm install (scripts/install.sh --vm), when it exists:
# gramine-vm and the Python tooling are then on PATH without sourcing env.sh by hand.
SAGUARO_ENV='[ -f "$HOME/.local/saguaro-vm/env.sh" ] && . "$HOME/.local/saguaro-vm/env.sh"'
if [ $# -gt 0 ]; then
    exec podman exec -i -w "$MOUNT_POINT" "$CONTAINER" bash -c "$SAGUARO_ENV; $*"
else
    exec podman exec -it -w "$MOUNT_POINT" "$CONTAINER" bash -c "$SAGUARO_ENV; exec bash"
fi
