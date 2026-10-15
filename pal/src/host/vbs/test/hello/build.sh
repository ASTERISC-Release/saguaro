#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Build the hello-world application as a static PIE against the vbs glibc (built with -ffixed-x18
# by scripts/vbs/build-glibc.sh; no syscall patch, the packer rewrites its `svc`).
# Usage: build.sh OUTDIR [SYSROOT]   (SYSROOT default: <repo>/build-vbs-glibc)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../../.." && pwd)
OUT=$(realpath "${1:-.}")
export SYSROOT=$(realpath "${2:-$ROOT/build-vbs-glibc}")
"$ROOT/scripts/vbs/cc-static-pie.sh" -o "$OUT/hello" "$HERE/hello.c"
readelf -hlW "$OUT/hello" | grep -E 'Type:|LOAD|TLS|RELRO|INTERP'
