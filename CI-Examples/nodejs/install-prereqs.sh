#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Installs the host prerequisites for the nodejs example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_saguaro
apt_install nodejs   # installs /usr/bin/nodejs (the example runs it under Saguaro)
prereq_log "nodejs: prerequisites OK"
