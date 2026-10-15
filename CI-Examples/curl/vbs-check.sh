#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make VBS=1 check` (the HTTP server is started by the Makefile, on the WSL side, on 127.0.0.1:19111):
# curl in a Windows VBS enclave fetches the page over the loopback. The enclave's sockets are the
# Windows host's and the WSL server answers on Windows' loopback (localhost forwarding).
set -euo pipefail
: "${GRAMINE:=../../scripts/vbs/gramine-vbs}"

"$GRAMINE" net wait 19111 60
"$GRAMINE" curl -s http://127.0.0.1:19111/ > OUTPUT
grep -q "Hello World" OUTPUT && echo "[ Success 1/1 ]"
rm -f OUTPUT
echo "[ Success ]"
