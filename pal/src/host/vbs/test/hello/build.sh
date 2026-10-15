#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# Build the hello-world application as a static PIE with the AArch64 toolchain and glibc of the
# build container (glibc has no Gramine syscall patch on AArch64; the packer rewrites its `svc`).
# -ffixed-x18: x18 is the enclave TEB (our own code never touches it).
set -e
cd "$(dirname "$0")"
OUT=${1:-.}
${CC:-gcc} -O2 -static-pie -ffixed-x18 -Wl,-z,max-page-size=0x10000 -o "$OUT/hello" hello.c
readelf -hlW "$OUT/hello" | grep -E 'Type:|LOAD|TLS|RELRO|INTERP'
