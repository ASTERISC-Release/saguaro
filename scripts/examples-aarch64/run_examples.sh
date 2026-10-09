#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Usage: run_examples.sh [--prereqs] [examples-dir]   (default dir: ../../CI-Examples)
#   --prereqs   first run CI-Examples/install-all-prereqs.sh (apt/sudo; downloads mongod)
set -uo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
PREREQS=0; [ "${1:-}" = "--prereqs" ] && { PREREQS=1; shift; }
EX=${1:-$HERE/../../CI-Examples}
export PATH="$HOME/.local/bin:$PATH"
[ "$PREREQS" = 1 ] && "$EX/install-all-prereqs.sh"
. "$HERE/../gramine-env.sh"   # GRAMINE_PREFIX, PATH, PYTHONPATH
pass=0; fail=0
run() { local name=$1; shift
  if ( cd "$EX/$name" && eval "$@" ) >/tmp/ex-$name.log 2>&1; then echo "PASS $name"; pass=$((pass+1)); else echo "FAIL $name (see /tmp/ex-$name.log)"; fail=$((fail+1)); fi; }
run curl     'make check | grep -q "Success"'
run gcc      'make check'
run nodejs   'make check | grep -q Success && gramine-direct ./nodejs heavy.js | grep -q "HEAVY OK"'
run openjdk  'make >/dev/null && gramine-direct java MultiThreadMain | grep -q "Final Count is: 10000" && javac -d . Stress.java && gramine-direct java Stress | grep -q "STRESS OK"'
run r        'make check | grep -q Success'
run pytorch  'make >/dev/null && gramine-direct ./pytorch pytorchexample.py | grep -q Done'
# mongodb needs `mongod` on PATH (e.g. the official aarch64 tarball unpacked somewhere; needs
# pymongo: pip3 install --user --break-system-packages pymongo). Skipped when mongod is absent.
if command -v mongod >/dev/null; then
  run mongodb 'make >/dev/null && (setsid gramine-direct mongod >/tmp/mongod-gramine.log 2>&1 &) ; for i in $(seq 1 90); do ss -tln | grep -q ":27017 " && break; sleep 1; done; python3 mongo_client_test.py 27017 | grep -q "MONGO OK"; rc=$?; pkill -TERM -x loader; sleep 3; pkill -KILL -x loader; exit $rc'
else echo "SKIP mongodb (mongod not on PATH)"; fi
echo "passed=$pass failed=$fail"; [ $fail -eq 0 ]
