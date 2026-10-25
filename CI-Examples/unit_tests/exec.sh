#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run the tests of the unit_tests subdirectories on this host under the backend given (see
# ../common_tools/exec-common.sh): each is built and run in its own directory and passes on exit status 0
# (and its expected output, if it has one). align_test/ is not a test (it prints pointers and waits).
#
# UNIT_TESTS_SKIP="name ..." leaves the named tests out (reported as [SKIP]); UNIT_TESTS_ONLY="name ..." runs only
# those. test/quick.sh uses them to keep thread-stress, which is far too slow under QEMU software emulation (TCG),
# out of the smoke and regression tiers.
#
# Each entry: directory, name, program built by make, manifest, what the runner is given, expected output.
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
TESTS=(
    "mremap|mremap|helloworld|helloworld.manifest|./helloworld|ALL IN-PLACE TESTS PASSED"
    "multi-thread|thread-tls|thread-tls|thread-tls.manifest|./thread-tls|"
    "multi-thread|thread-rwlock|thread-rwlock|thread-rwlock.manifest|./thread-rwlock|"
    "multi-thread|thread-stress|thread-stress|thread-stress.manifest|./thread-stress|"
    "networks|networks|udp|nw.manifest|nw|UDP SOCKET TEST PASSED"
)

failed=0
for t in "${TESTS[@]}"; do
    IFS='|' read -r dir name binary manifest program expect <<<"$t"
    if [ -n "${UNIT_TESTS_ONLY:-}" ] && [[ " $UNIT_TESTS_ONLY " != *" $name "* ]]; then continue; fi
    if [[ " ${UNIT_TESTS_SKIP:-} " == *" $name "* ]]; then
        echo "[SKIP] unit_tests-$name: left out by UNIT_TESTS_SKIP"
        continue
    fi
    (
        EXEC_DIR=$HERE/$dir
        EXAMPLE_NAME=unit_tests-$name
        EXAMPLE_BINARY=$binary
        EXAMPLE_MANIFEST=$manifest
        EXAMPLE_PROGRAM=$program
        EXAMPLE_EXPECT=$expect
        EXAMPLE_ARGS=()
        EXAMPLE_SKIP_UNSUPPORTED=1
        EXAMPLE_BACKENDS="direct vm sev"
        EXAMPLE_ARCHES="x86_64 aarch64"
        . "$HERE/../common_tools/exec-common.sh"
        exec_main "$@"
    )
    rc=$?
    [ $rc = 2 ] && exit 2    # usage error: said once, not once per test
    [ $rc = 0 ] || failed=1
done
exit $failed
