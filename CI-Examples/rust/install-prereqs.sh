#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Installs the host prerequisites for the rust example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
#   direct, vm, sev, sev-snp: cargo and rustc newer than the distribution's 1.75 (the crates need edition
#                             2024), curl for the check; the packages put cargo in /usr/lib/rust-1.85/bin
#   vbs:                      rustup's stable toolchain with the rust-src component (-Zbuild-std,
#                             -Zfixed-x18), in ~/.cargo/bin. rustup is installed only with
#                             WITH_RUSTUP=1 (it runs the installer of https://sh.rustup.rs); without it the
#                             script prints the commands.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"

RUST=$(first_available_apt cargo-1.85 cargo) || prereq_die "no cargo package found in apt"
RUSTC=$(first_available_apt rustc-1.85 rustc) || prereq_die "no rustc package found in apt"
prereq_log "using $RUST and $RUSTC"
apt_install "$RUST" "$RUSTC" curl build-essential
case $RUST in
    cargo-1.85) prereq_log "note: put /usr/lib/rust-1.85/bin first in PATH (the container images do)" ;;
    *) prereq_log "WARNING: only the distribution's cargo is available; the crates of the example need a newer one" ;;
esac

# the VBS build
export PATH="$HOME/.cargo/bin:$PATH"
if command -v rustup >/dev/null; then
    prereq_log "rustup found: $(command -v rustup)"
    if [ "${DRY_RUN:-0}" = 1 ]; then
        prereq_log "(dry run) rustup toolchain install stable; rustup component add rust-src"
    else
        rustup toolchain install stable --profile minimal
        rustup component add rust-src
    fi
elif [ "${WITH_RUSTUP:-0}" = 1 ]; then
    prereq_log "installing rustup (https://sh.rustup.rs)"
    [ "${DRY_RUN:-0}" = 1 ] && { prereq_log "(dry run) curl https://sh.rustup.rs | sh -s -- -y --profile minimal"; exit 0; }
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
    "$HOME/.cargo/bin/rustup" component add rust-src
else
    prereq_log "VBS only: rustup is not installed. Install it with   WITH_RUSTUP=1 $0"
    prereq_log "or by hand:  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal"
    prereq_log "              ~/.cargo/bin/rustup component add rust-src"
fi
prereq_log "rust: prerequisites OK"
