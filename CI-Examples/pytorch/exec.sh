#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run pytorch on this host under the backends installed here (see ../common_tools/exec-common.sh).
EXAMPLE_BINARY=pytorch
EXAMPLE_BUILD=all
EXAMPLE_MANIFEST=pytorch.manifest
EXAMPLE_ARGS=(./pytorchexample.py)
EXAMPLE_EXPECT="Labrador retriever"
EXAMPLE_EXPECT_FILE=result.txt   # the classifier writes its result there
EXAMPLE_BACKENDS="direct vm sev"
EXAMPLE_REQUIRES="python3"

. "$(dirname "$(readlink -f "$0")")/../common_tools/exec-common.sh"
exec_main "$@"
