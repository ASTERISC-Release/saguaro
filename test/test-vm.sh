#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

exec "$(dirname "$(readlink -f "$0")")/vm/test-vm.sh" "$@"
