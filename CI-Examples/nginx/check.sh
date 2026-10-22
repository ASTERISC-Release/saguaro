#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# `make check` (direct, vm and sev): nginx runs under $SAGUARO and curl on the host fetches the 10 KiB
# test page over HTTP (8002) and HTTPS (8444, self-signed); both must be byte-identical to the file.
# Under the vm and sev nginx runs as one process: the guest cannot create worker processes, so
# `master_process off;` is added to the installed configuration.
set -uo pipefail
: "${SAGUARO:=saguaro-direct}"
. ../common_tools/server-check.sh
case $SAGUARO in
    saguaro-vm|saguaro-sev)
        grep -q '^master_process' install/conf/nginx-saguaro.conf \
            || sed -i '1i master_process off;' install/conf/nginx-saguaro.conf ;;
esac
server_start 8002:8002,8444:8444 "$SAGUARO" ./nginx
fetch() {
    curl -sf --max-time 5 http://127.0.0.1:8002/random/10K.1.html -o fetched-http.html \
        && curl -sfk --max-time 5 https://127.0.0.1:8444/random/10K.1.html -o fetched-https.html
}
retry 120 fetch
rc=$?
if [ $rc = 0 ]; then
    cmp fetched-http.html install/html/random/10K.1.html \
        && cmp fetched-https.html install/html/random/10K.1.html; rc=$?
fi
rm -f fetched-http.html fetched-https.html
server_stop
[ $rc = 0 ] && rm -f server.log
[ $rc = 0 ] && echo "nginx: HTTP and HTTPS, 10240 bytes, identical to the file" && echo "[ Success 1/1 ]"
exit $rc
