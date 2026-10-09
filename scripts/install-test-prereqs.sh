#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Installs the host prerequisites for building Gramine and running ./run_all_tests.sh
# (Debian/Ubuntu). Idempotent: only missing packages are installed; sudo may prompt for a password.
#   DRY_RUN=1            preview without installing
#   INSTALL_RUSTUP=1     also install a Rust toolchain via rustup when `cargo` is missing
#                        (the Ubuntu cargo is too old for the rust example; needs network)
#
# Per-example prerequisites for the other CI-Examples are in CI-Examples/*/install-prereqs.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../CI-Examples/common_tools/prereqs.sh"

# Build + runtime dependencies of Gramine itself (see install.sh) ...
apt_install build-essential meson ninja-build pkg-config bison gawk nasm file procps iproute2 \
    linux-libc-dev python3 python3-pip python3-pytest python3-click python3-jinja2 \
    python3-cryptography python3-voluptuous python3-toml python3-tomli python3-tomli-w

# ... and of the example suites in the battery:
#   memcached: libevent-dev   sqlite: sqlite3   nginx (bundled OpenSSL/PCRE build): autoconf automake
#   downloads/patching: curl wget patch   nginx/curl readiness probe: netcat-openbsd
apt_install libevent-dev sqlite3 autoconf automake curl wget patch netcat-openbsd

# python example (numpy/scipy checks)
pip_user_install numpy numpy
pip_user_install scipy scipy

# rust example
if command -v cargo >/dev/null || [ -x "$HOME/.cargo/bin/cargo" ]; then
    prereq_log "cargo found"
elif [ "${INSTALL_RUSTUP:-0}" = 1 ]; then
    prereq_log "installing a Rust toolchain via rustup"
    if [ "${DRY_RUN:-0}" = 1 ]; then
        prereq_log "(dry run) curl https://sh.rustup.rs | sh -s -- -y --profile minimal"
    else
        curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal
    fi
else
    prereq_log "WARNING: cargo not found; the 'rust' suite will fail. Re-run with INSTALL_RUSTUP=1" \
               "(or install rustup yourself) and put ~/.cargo/bin on PATH."
fi
prereq_log "test prerequisites OK"
