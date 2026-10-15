#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: the regression of this example (create, update, select) in a Windows VBS
# enclave. The scripts are read with `.read` from the command line (the enclave has no console
# input); the output is compared with scripts/select.txt. GRAMINE is gramine-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"

"$GRAMINE" sqlite3 /db/test.db ".read /scripts/create.sql"
echo "[ Success 1/3 ]"

"$GRAMINE" sqlite3 /db/test.db ".read /scripts/update.sql"
echo "[ Success 2/3 ]"

"$GRAMINE" sqlite3 /db/test.db ".read /scripts/select.sql" > OUTPUT
diff OUTPUT scripts/select.txt
echo "[ Success 3/3 ]"
echo "[ Success ]"
