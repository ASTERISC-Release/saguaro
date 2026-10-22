#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check`: the three SQL scripts run in a Windows VBS enclave, one enclave per script
# (`sqlite3 DB ".read SCRIPT"`), the last output is compared with scripts/select.txt. GRAMINE is
# saguaro-vbs.
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/saguaro-vbs}"
DB=/db/test.db

"$GRAMINE" sqlite3 "$DB" ".read /scripts/create.sql"
echo "[ Success 1/3 ]"

"$GRAMINE" sqlite3 "$DB" ".read /scripts/update.sql"
echo "[ Success 2/3 ]"

"$GRAMINE" sqlite3 "$DB" ".read /scripts/select.sql" > OUTPUT
diff OUTPUT scripts/select.txt
echo "[ Success 3/3 ]"
echo "[ Success ]"
