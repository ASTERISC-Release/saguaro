#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Runs every example's install-prereqs.sh (or only the named examples). DRY_RUN=1 to preview.
#   CI-Examples/install-all-prereqs.sh            # curl gcc iperf mongodb nodejs openjdk pytorch r
#   CI-Examples/install-all-prereqs.sh nodejs r
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
EXAMPLES=("$@")
[ ${#EXAMPLES[@]} -gt 0 ] || EXAMPLES=(curl gcc iperf mongodb nodejs openjdk pytorch r)
for ex in "${EXAMPLES[@]}"; do
    echo "=== $ex"
    "$HERE/$ex/install-prereqs.sh"
done
