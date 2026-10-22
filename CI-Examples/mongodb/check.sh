#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): mongod runs under $GRAMINE and mongo_client_test.py (pymongo, see
# install-prereqs.sh) runs on the host against port 27017: the example's insert/fetch, a bulk insert
# with indexes and an aggregation, and eight concurrent clients.
set -uo pipefail
: "${GRAMINE:=saguaro-direct}"
. ../common_tools/server-check.sh
python3 -c 'import pymongo' 2>/dev/null || { echo "FAIL: pymongo is missing (./install-prereqs.sh)"; exit 1; }
rm -rf data/db/*; mkdir -p data/db
server_start 27017:27017 "$GRAMINE" mongod
timeout 600 python3 mongo_client_test.py 27017
rc=$?
server_stop
if [ $rc = 0 ]; then rm -f server.log; echo "[ Success 1/1 ]"; else tail -n 20 server.log; fi
exit $rc
