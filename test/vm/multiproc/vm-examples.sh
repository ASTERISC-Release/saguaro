#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Run the CI examples under saguaro-vm, optionally with sys.experimental__multiprocess = true
# (MP=1) added to their generated manifests (restored afterwards). Run in the container after
# `. <prefix>/env.sh`. Servers go through test/direct/test-direct.sh with SAGUARO_RUNNER=saguaro-vm; the
# other examples (which test-direct.sh runs with saguaro-direct) are run here with saguaro-vm.
# Usage: [MP=1] [CPUS=2] [REPO=...] test/vm/multiproc/vm-examples.sh
set -u
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
MP=${MP:-0}
export SAGUARO_CPU_NUM=${CPUS:-2}
OUT=${OUT:-/tmp/vm-examples-mp$MP}
mkdir -p "$OUT"; : > "$OUT/patched"
E=$REPO/CI-Examples

patch() {  # patch <manifest>
    [ "$MP" = 1 ] || return 0
    local m=$1
    cp "$m" "$m.mp-bak"; echo "$m" >> "$OUT/patched"
    if grep -q '^\[sys\]$' "$m"; then
        sed -i 's/^\[sys\]$/[sys]\nexperimental__multiprocess = true/' "$m"
    else
        printf '\n[sys]\nexperimental__multiprocess = true\n' >> "$m"
    fi
}
restore() { while read -r m; do [ -f "$m.mp-bak" ] && mv "$m.mp-bak" "$m"; done < "$OUT/patched"; }
trap restore EXIT

res() { printf '%-5s %s %s\n' "$1" "$2" "${3:-}"; }

for m in "$E"/helloworld/helloworld.manifest "$E"/unit_tests/multi-thread/thread-{tls,rwlock,stress}.manifest \
         "$E"/busybox/busybox.manifest "$E"/bash/bash.manifest "$E"/sqlite/sqlite3.manifest \
         "$E"/python/python.manifest "$E"/redis/redis-server.manifest "$E"/memcached/memcached.manifest \
         "$E"/lighttpd/lighttpd.manifest "$E"/nginx/nginx.manifest "$E"/rust/rust-hyper-http-server.manifest \
         "$E"/unit_tests/networks/*.manifest; do
    [ -f "$m" ] && patch "$m"
done

( cd "$E/helloworld" && timeout -s KILL 60 saguaro-vm helloworld ) > "$OUT/helloworld.log" 2>&1
grep -q "Hello, world" "$OUT/helloworld.log" && res PASS helloworld || res FAIL helloworld

rc=0
for b in thread-tls thread-rwlock thread-stress; do
    ( cd "$E/unit_tests/multi-thread" && timeout -s KILL 120 saguaro-vm $b ) > "$OUT/$b.log" 2>&1 || rc=1
    grep -q "VM exited with code 0" "$OUT/$b.log" || rc=1
done
[ $rc = 0 ] && res PASS multithread || res FAIL multithread

( cd "$E/busybox" && timeout -s KILL 60 saguaro-vm busybox sh -c 'echo busybox-shell-works; uname -m' ) \
    > "$OUT/busybox.log" 2>&1
grep -q busybox-shell-works "$OUT/busybox.log" && grep -qx x86_64 "$OUT/busybox.log" \
    && res PASS busybox || res FAIL busybox

vmfilter() {  # drop the VM launcher/PAL lines that the single guest console mixes into stdout
    grep -v -e '^Configuring Saguaro VM' -e '^REM_MEM_SIZE' -e '^\[boot\]' -e '^(PAL) Physical' \
            -e '^Private frame pool' -e 'self-check passed' -e '^\[ VM ' -e '^vm-relay:'
}

# bash: the example's own 7 checks (its Makefile already renders and runs bash.manifest; checks
# 2..7 run "bash bash_test.sh", a second process)
( cd "$E/bash" && rm -f OUTPUT; timeout -s KILL 300 make regression GRAMINE=saguaro-vm ) > "$OUT/bash.log" 2>&1
n=$(grep -ac '^\[ Success [0-9]*/7 \]' "$OUT/bash.log"); [ "$n" = 7 ] && res PASS bash "$n/7" || res FAIL bash "$n/7"

# sqlite: the example's create/update/select scripts, the output compared without VM console lines
( cd "$E/sqlite" && rm -f db/*
  timeout -s KILL 120 saguaro-vm sqlite3 < scripts/create.sql >/dev/null 2>&1 \
  && timeout -s KILL 120 saguaro-vm sqlite3 < scripts/update.sql >/dev/null 2>&1 \
  && timeout -s KILL 120 saguaro-vm sqlite3 < scripts/select.sql 2>/dev/null | vmfilter > "$OUT/sqlite.out" \
  && diff "$OUT/sqlite.out" scripts/select.txt ) > "$OUT/sqlite.log" 2>&1 \
    && res PASS sqlite "create/update/select" || res FAIL sqlite "see $OUT/sqlite.log"

# python: run-tests.sh in its VM mode (server reached through the relay on 8005)
( cd "$E/python" && GRAMINE=saguaro-vm SAGUARO_VM=1 SAGUARO_VM_INBOUND=8005:8005 \
      timeout -s KILL 900 ./run-tests.sh > TEST_STDOUT 2> TEST_STDERR ) > "$OUT/python.log" 2>&1
n=$(grep -ac '^\[ Success [0-9]*/4 \]' "$E/python/TEST_STDOUT" 2>/dev/null); [ "$n" = 4 ] && res PASS python "$n/4" || res FAIL python "$n/4"

[ "${SERVERS:-1}" = 1 ] || exit 0
( cd "$REPO" && SAGUARO_RUNNER=saguaro-vm test/direct/test-direct.sh redis memcached lighttpd nginx rust networks ) \
    > "$OUT/servers.log" 2>&1
sed -n '/SUMMARY/,/====/p' "$OUT/servers.log" | grep -E "^(PASS|FAIL)"
