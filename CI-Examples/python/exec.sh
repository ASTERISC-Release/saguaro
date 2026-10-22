#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run python on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_MODE=check
EXAMPLE_MANIFEST=python.manifest
EXAMPLE_ARCHES="x86_64 aarch64"
EXAMPLE_BACKENDS="direct vm sev"

# Under the vm and sev, run-tests.sh serves the HTTP page in the guest through the relay (the port is
# forwarded by SAGUARO_VM_INBOUND) and runs its client on the host (README)
example_check_args() {
    case $1 in vm|sev) echo "SAGUARO_VM=1 SAGUARO_VM_INBOUND=8005:8005" ;; esac
}

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
