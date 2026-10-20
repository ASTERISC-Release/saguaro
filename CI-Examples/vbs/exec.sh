#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Run the examples of the vbs subdirectories for a Windows VBS enclave (see
# ../common_tools/exec-common.sh): ./exec.sh --vbs, in WSL on a Windows machine with VBS enclaves and
# the runtime installed (scripts/install.sh --vbs). Each subdirectory is built with `make VBS=1` and
# checked with `make VBS=1 check` in its own directory. These examples are for the VBS backend only.
# common/ holds sources shared by the examples (the RA-TLS helper) and is not an example. jvmprobe is left
# out: it is an x64-only research probe (Temurin 21) that is "not expected to work" (its Makefile).
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
EXAMPLES=(helloworld dynhello python tcpserver attestation tls-echo jitprobe rxprobe)

failed=0
for name in "${EXAMPLES[@]}"; do
    (
        EXEC_DIR=$HERE/$name
        EXAMPLE_NAME=vbs-$name
        EXAMPLE_BACKENDS="vbs"
        EXAMPLE_ARCHES="aarch64 x86_64"
        . "$HERE/../common_tools/exec-common.sh"
        exec_main "$@"
    )
    rc=$?
    [ $rc = 2 ] && exit 2    # usage error: said once, not once per example
    [ $rc = 0 ] || failed=1
done
exit $failed
