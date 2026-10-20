#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# run.sh -- one entry point for the test runners of test/ (dev docs: test-layout/).
#
# Usage:
#   test/run.sh list                      # the backends and the suites each accepts
#   test/run.sh <backend> [args...]       # run the runner of the backend; args pass through unchanged
#
# Backends:
#   direct    test/direct/test-direct.sh
#   vm        test/vm/test-vm.sh
#   sev       test/sev/test-sev.sh
#   sev-all   test/sev/test-sev-all.sh   (the SEV qualification run; it takes no suite)
#
# The dispatcher changes no default and no existing script. Set RUN_DRY=1 to print the command
# instead of running it.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)

runner_of() {
    case $1 in
        direct)  echo "$HERE/direct/test-direct.sh" ;;
        vm)      echo "$HERE/vm/test-vm.sh" ;;
        sev)     echo "$HERE/sev/test-sev.sh" ;;
        sev-all) echo "$HERE/sev/test-sev-all.sh" ;;
        *)       return 1 ;;
    esac
}

# Suite names, read from the runner's own text: direct has --list; vm and sev list them in the
# header of their --help, as lines "  <name>   <description>" before the "Usage:" line.
suites_of() {
    case $1 in
        direct)  "$HERE/direct/test-direct.sh" --list 2>/dev/null | tr '\n' ' ' ;;
        vm|sev)  "$(runner_of "$1")" --help 2>/dev/null \
                     | sed -n '/^Usage:/q; s/^  \([a-z][a-z-]*\) \{1,\}[^ ].*/\1/p' | tr '\n' ' ' ;;
        sev-all) printf '(none: one qualification run) ' ;;
    esac
}

usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; }

[ $# -gt 0 ] || { usage >&2; exit 2; }
case $1 in
    -h|--help) usage; exit 0 ;;
    list)
        for b in direct vm sev sev-all; do
            printf '%-8s %s\n         suites: %s\n' "$b" "test/$(basename "$(runner_of "$b")")" "$(suites_of "$b")"
        done
        exit 0 ;;
esac

backend=$1; shift
runner=$(runner_of "$backend") || { echo "unknown backend: $backend (test/run.sh list)" >&2; exit 2; }
if [ "${RUN_DRY:-0}" = 1 ]; then
    printf '%q' "$runner"; [ $# -eq 0 ] || printf ' %q' "$@"; echo
    exit 0
fi
exec "$runner" "$@"
