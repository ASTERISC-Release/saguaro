#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run openjdk on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_BINARY=java
EXAMPLE_PROGRAM=java
EXAMPLE_BUILD=all
EXAMPLE_MANIFEST=java.manifest
EXAMPLE_ARGS=(MultiThreadMain)
EXAMPLE_EXPECT="Final Count is:"
EXAMPLE_BACKENDS="direct vm sev"
EXAMPLE_REQUIRES="java"

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
