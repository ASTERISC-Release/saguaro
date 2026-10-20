#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run curl on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_MODE=check
EXAMPLE_MANIFEST=curl.manifest
EXAMPLE_ARCHES="x86_64 aarch64"
EXAMPLE_BACKENDS="direct vm sev"

# The check's web server must be on an address the guest can reach: not the host's loopback, which
# in a vm or sev guest is the guest itself (README footnote 3).
example_check_args() {
    case $1 in vm|sev) echo "HTTP_ADDR=$(hostname -I | awk '{print $1}')" ;; esac
}

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
