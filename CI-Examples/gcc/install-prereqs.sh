#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Installs the host prerequisites for the gcc example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_gramine
apt_install build-essential patch wget   # gcc/binutils run inside Gramine; patch+wget fetch and fix the test sources
prereq_log "gcc: prerequisites OK"
