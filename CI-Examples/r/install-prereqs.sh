#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Installs the host prerequisites for the r example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_gramine
apt_install r-base-core   # R_HOME defaults to /usr/lib/R
prereq_log "r: prerequisites OK"
