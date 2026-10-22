#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# `make VBS=1 check`: the enclave writes an attestation report for given user data and the
# verifier accepts it for that data and rejects it for other data (dev docs: vbs-enclave/manual.md, Attestation).
set -euo pipefail
: "${SAGUARO:=../../../scripts/vbs/saguaro-vbs}"
N=$(printf 'ab%.0s' $(seq 32)); O=$(printf 'cd%.0s' $(seq 32))
"$SAGUARO" attest "$N" > OUTPUT 2>&1
grep -q "attest test: passed" OUTPUT || { cat OUTPUT; echo "FAIL: attest"; exit 1; }
"$SAGUARO" verify attest data/out/report.bin --user-data "$N" --same-image --min-svn 1 | tr -d '\r' | tee -a OUTPUT | grep -q "result: VERIFIED" \
    || { cat OUTPUT; echo "FAIL: verify of the right user data"; exit 1; }
if "$SAGUARO" verify attest data/out/report.bin --user-data "$O" --same-image >> OUTPUT 2>&1; then
    echo "FAIL: other user data accepted"; exit 1
fi
echo "[ Success ]"
