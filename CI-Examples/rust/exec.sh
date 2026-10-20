#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run rust on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_MODE=check
EXAMPLE_MANIFEST=rust-hyper-http-server.manifest
EXAMPLE_BACKENDS="direct vm sev"
EXAMPLE_REQUIRES="cargo curl"
EXAMPLE_REQUIRES_VBS="rustup cargo"

# the VBS build needs the rustup toolchain (stable with rust-src) of ~/.cargo/bin, not the distribution's rustc
PATH=$HOME/.cargo/bin:$PATH
EXAMPLE_ARCHES="x86_64 aarch64"

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
