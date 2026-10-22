#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# The LibOS regression subset in a VBS enclave, from WSL on the Windows machine (TESTING.md of
# dev docs: vbs-enclave): builds the programs (build.sh), packs them into one image
# (vbs.manifest.template), signs it on Windows, and runs every case of cases.txt in its own host
# process with a 60 s watchdog (run-regress.ps1). Pass = exit code 0 and "TEST OK" in the output.
# Usage: run-regress.sh OUTDIR [CASE-FILTER-REGEX]   (SAGUARO_REPO: another checkout to test)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${SAGUARO_REPO:-$(cd "$HERE/../../../../../.." && pwd)}
OUT=$(realpath -m "$1")
FILTER=${2:-.}
G="$ROOT/scripts/vbs/saguaro-vbs"
"$G" runtime
sh "$ROOT/pal/src/host/vbs/test/regress/build.sh" "$OUT"
tests=$(cd "$OUT/tests" && ls | tr '\n' ' ')
"$G" manifest -Darch_libdir=/usr/lib -Dout="$OUT" -Dtests="$tests" \
    "$HERE/vbs.manifest.template" "$OUT/regress.manifest.vbs"
"$G" build "$OUT/regress.manifest.vbs"
windir=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['windows_dir'])" \
         "$OUT/regress.vbs/build.json")
grep -E "$FILTER" "$HERE/cases.txt" > "$OUT/cases.txt" || true
cp "$HERE/run-regress.ps1" "$(wslpath "$windir")/run-regress.ps1"
cp "$OUT/cases.txt" "$(wslpath "$windir")/cases.txt"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windir\\run-regress.ps1" -Dir "$windir" \
    -Name regress | tr -d '\r' | tee "$OUT/results.txt"
