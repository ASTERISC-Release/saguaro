#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Installs the host prerequisites for the blender example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
# The Makefile uses lsb_release and downloads the Blender 2.82 binary itself.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_gramine
apt_install lsb-release
prereq_log "note: make downloads the Blender 2.82 tarball (about 170 MB)"
prereq_log "blender: prerequisites OK"
