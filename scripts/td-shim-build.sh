#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# td-shim-build.sh -- build td-shim the way the install step of the TDX PAL needs it.
#
#   scripts/td-shim-build.sh [DIR]      DIR: the td-shim checkout (default $HOME/td-shim)
#
# pal/src/host/tdx/meson.build links the installed PAL into the TD image with td-shim-ld, from
# target/x86_64-unknown-none/release/ResetVector.bin and target/x86_64-unknown-none/release/td-shim of
# $HOME/td-shim (https://github.com/confidential-containers/td-shim, the commit below). The PAL is an unsigned
# payload, so td-shim is built without its secure-boot feature (--no-default-features --features=main,tdx).
# Needs rustup (the checkout's rust-toolchain names the toolchain; the target x86_64-unknown-none is added here),
# nasm, clang and llvm-ar. Idempotent: a second run rebuilds nothing.
set -euo pipefail
TD_SHIM_COMMIT=54a862b249a62248b726cd3dc4a96421fdc36d66
DIR=${1:-$HOME/td-shim}
cd "$DIR"
head=$(git rev-parse HEAD)
[ "$head" = "$TD_SHIM_COMMIT" ] || echo "note: td-shim is at ${head:0:12}, the commit this tree was tested with is ${TD_SHIM_COMMIT:0:12}" >&2
git submodule update --init --recursive
sh_script/preparation.sh
if command -v rustup >/dev/null; then rustup target add x86_64-unknown-none; fi
export CC=clang AR=llvm-ar CC_x86_64_unknown_none=clang AR_x86_64_unknown_none=llvm-ar
cargo build -p td-shim --target x86_64-unknown-none --release --no-default-features --features=main,tdx
cargo build -p td-shim-tools --release --features=linker
ls -l target/x86_64-unknown-none/release/ResetVector.bin target/x86_64-unknown-none/release/td-shim target/release/td-shim-ld
