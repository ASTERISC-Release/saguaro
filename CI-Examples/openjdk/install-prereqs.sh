#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Installs the host prerequisites for the openjdk example (Debian/Ubuntu). Idempotent; DRY_RUN=1 to preview.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$HERE/../common_tools/prereqs.sh"
check_gramine
JDK=$(first_available_apt openjdk-17-jdk-headless openjdk-21-jdk-headless openjdk-11-jdk-headless) \
    || prereq_die "no OpenJDK package found in apt"
prereq_log "using $JDK"
apt_install "$JDK"
prereq_log "openjdk: prerequisites OK"
