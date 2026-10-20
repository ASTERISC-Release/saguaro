#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run mongodb on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_MODE=check
EXAMPLE_MANIFEST=mongod.manifest
EXAMPLE_BACKENDS="direct vm sev"
EXAMPLE_REQUIRES="mongod python3"

# install-prereqs.sh links mongod into ~/.local/bin; the Makefile finds it with `which`
PATH=$HOME/.local/bin:$PATH

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
