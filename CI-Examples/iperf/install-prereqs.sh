#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Installs the host prerequisites for the iperf example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_gramine
apt_install build-essential wget   # iperf3 is built from source (tarball fetched with wget)
prereq_log "iperf: prerequisites OK"
