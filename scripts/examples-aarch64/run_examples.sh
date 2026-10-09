#!/usr/bin/env bash
# Usage: run_examples.sh <path-to-gramineproject/examples checkout (patch applied)>
set -uo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
EX=${1:?usage: $0 <examples-dir>}
GRAMINE_PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine}
export PATH="$GRAMINE_PREFIX/bin:$PATH"
export PYTHONPATH="$GRAMINE_PREFIX/lib/python3.12/site-packages${PYTHONPATH:+:$PYTHONPATH}"
cp "$HERE/heavy.js" "$EX/nodejs/"; cp "$HERE/Stress.java" "$EX/openjdk/"; cp "$HERE/mongo_client_test.py" "$EX/mongodb/"
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
