#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run-checks.sh — build and run the standalone arch checks in scripts/riscv64/checks/.
#
# riscv64 is the target of the port: the checks cross-compile here and run in the full-system
# guest (scripts/riscv64/guest.sh), never under user-mode emulation (it has no seccomp and
# misrepresents everything the checks are about — see the Phase 0 log entry). x86_64 runs the
# same drivers natively as a semantics oracle: the riscv64 assembly must agree with the
# reference implementation the x86 build already passes with.
#
# Usage:
#   scripts/riscv64/run-checks.sh                     # riscv64: build + run all in the guest
#   scripts/riscv64/run-checks.sh --target x86_64     # native build + run
#   scripts/riscv64/run-checks.sh [name...]           # only the named checks
#
# The riscv64 mode requires the cross container (container/run.sh --target riscv64) and a
# running guest; build outputs go to the git-ignored .riscv64-checks/ *inside the repository*,
# because that is the only tree both the container and the guest (via 9p) can see.

set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(dirname "$(dirname "$HERE")")
TARGET=riscv64
NAMES=()

while [ $# -gt 0 ]; do
    case "$1" in
        --target) TARGET=$2; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) NAMES+=("$1"); shift ;;
    esac
done

case $TARGET in
    riscv64)
        CC=riscv64-linux-gnu-gcc
        ARCH=riscv64
        ;;
    x86_64)
        CC=gcc
        ARCH=x86_64
        ;;
    aarch64)
        CC=aarch64-linux-gnu-gcc
        ARCH=aarch64
        ;;
    *) echo "unknown --target: $TARGET" >&2; exit 2 ;;
esac

BUILDDIR=$REPO/.riscv64-checks
mkdir -p "$BUILDDIR"

# Per-check extra sources: the hand-written assembly under test is linked from its real
# repository location (that exact file is what libpal.so/libsysdb.so embed).
extra_sources() {
    case $1 in
        common_arch)
            if [ "$ARCH" = riscv64 ]; then
                echo "$REPO/common/src/arch/riscv64/ct_memequal.S"
                echo "$REPO/pal/src/host/linux-common/arch/riscv64/stack_probe.S"
            elif [ "$ARCH" = x86_64 ]; then
                # same nasm invocation as the meson `nasm_gen` generator (the pre-included
                # no_exec_stack.nasm is what marks objects as non-executable-stack)
                objects=()
                for n in common/src/arch/x86_64/ct_memequal pal/src/host/linux-common/arch/x86_64/stack_probe; do
                    (cd "$BUILDDIR" && nasm -f elf64 -p "$REPO/common/src/arch/x86_64/no_exec_stack.nasm" \
                        "$REPO/$n.nasm" -o "$(basename "$n").o")
                    objects+=("$BUILDDIR/$(basename "$n").o")
                done
                printf '%s\n' "${objects[@]}"
            else
                echo "$REPO/common/src/arch/aarch64/ct_memequal.S"
                echo "$REPO/pal/src/host/linux-common/arch/aarch64/stack_probe.S"
            fi
            ;;
        *) ;;
    esac
}

run_one() {
    local name=$1
    local src=$HERE/checks/$name.c
    local out=$BUILDDIR/$name.$ARCH
    local extras
    extras=$(extra_sources "$name")

    echo ">>> $name ($ARCH)"
    # shellcheck disable=SC2086
    $CC -static -O2 -Wall -o "$out" "$src" \
        -I "$REPO/common/include" -I "$REPO/common/include/arch/$ARCH" \
        $extras

    if [ "$TARGET" = riscv64 ]; then
        # the guest mounts the repository at the same path; the binary lives inside it
        "$REPO/scripts/riscv64/guest.sh" ssh "cd $REPO && .riscv64-checks/$(basename "$out")"
    else
        "$out"
    fi
}

if [ ${#NAMES[@]} -eq 0 ]; then
    for f in "$HERE"/checks/*.c; do
        NAMES+=("$(basename "$f" .c)")
    done
fi

rc=0
for name in "${NAMES[@]}"; do
    if ! run_one "$name"; then
        echo ">>> $name: FAILED" >&2
        rc=1
    fi
done
exit $rc
