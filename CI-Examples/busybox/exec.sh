#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run busybox on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_MODE=check
EXAMPLE_MANIFEST=busybox.manifest
EXAMPLE_ARCHES="x86_64 aarch64"
EXAMPLE_BACKENDS="direct vm sev"

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
