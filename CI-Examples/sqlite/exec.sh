#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run sqlite on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_MODE=check
EXAMPLE_CHECK_TARGET=regression
EXAMPLE_MANIFEST=sqlite3.manifest
EXAMPLE_BACKENDS="direct vm sev"
EXAMPLE_REQUIRES="sqlite3"
EXAMPLE_REQUIRES_VBS="unzip"
EXAMPLE_ARCHES="x86_64 aarch64"

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
