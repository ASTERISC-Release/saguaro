#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Installs the host prerequisites for the sqlite example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
#   direct, vm, sev, sev-snp: the sqlite3 program (the manifest runs the host's binary) and a compiler
#   vbs:                      unzip (the Makefile builds sqlite3 from the amalgamation zip for the enclave)
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
apt_install sqlite3 unzip build-essential
prereq_log "sqlite: prerequisites OK"
