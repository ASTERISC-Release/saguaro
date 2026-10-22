#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Installs the host prerequisites for the pytorch example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_saguaro
apt_install python3 python3-venv python3-pip lsb-release
# The upstream README also lists libnss-mdns/libnss-myhostname (hostname resolution); the example
# runs without them, so they are opt-in: WITH_NSS=1 ./install-prereqs.sh
[ "${WITH_NSS:-0}" = 1 ] && apt_install libnss-mdns libnss-myhostname || true
prereq_log "note: the Makefile installs CPU-only torch/torchvision into ./my_venv (~900 MB) and downloads the AlexNet model"
prereq_log "pytorch: prerequisites OK"
