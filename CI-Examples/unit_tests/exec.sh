#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run unit_tests on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_BACKENDS=""
EXAMPLE_NOTE="no top-level Makefile; each test runs from its own directory"

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
