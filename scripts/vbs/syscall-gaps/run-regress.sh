#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# The 104 LibOS regression cases (pal/src/host/vbs/test/regress/cases.txt) in a VBS enclave, via
# the existing run-regress.sh, then a summary of failed cases for the syscall-gap table.
# Usage: run-regress.sh OUTDIR
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/../../../pal/src/host/vbs/test/regress/run-regress.sh" "$1" 2>&1 | tee "$1.log"
grep -i -E 'fail|hang|timeout' "$1.log" | sort | uniq -c | sort -rn | head -80
