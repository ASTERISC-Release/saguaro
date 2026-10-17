#!/usr/bin/env bash
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# The linker of cargo for `make VBS=1` (CARGO_TARGET_<TRIPLE>_LINKER): the server and its crates are
# linked against the vbs glibc (scripts/vbs/cc-static-pie.sh). The target triple is the host's, so
# cargo's host programs (build scripts, and the proc-macro crates, which are shared objects loaded by
# rustc) are linked with the host gcc: they are recognised by their output (`-shared`, or the build
# script folder). Needs SYSROOT (the vbs glibc sysroot) and RUST_VBS_CC_STATIC (cc-static-pie.sh).
set -euo pipefail
case " $* " in
    *" -shared "*|*"/release/build/"*) exec gcc "$@" ;;
esac
# The sysroot's libm.a and libc.so are linker scripts with absolute host paths (/usr/lib/...), so the
# libraries cargo asks for are mapped here: libm to its archives in the sysroot, libgcc_s to the static
# libgcc_eh (no shared libgcc_s in the image), and the libraries merged into libc (glibc 2.34+) are
# dropped. cc-static-pie.sh adds -lc after these arguments: -Bstatic (last) makes it take libc.a, not
# the libc.so script.
args=()
for a in "$@"; do
    case "$a" in
        -lm) args+=("$SYSROOT/usr/lib/libm-2.39.a" "$SYSROOT/usr/lib/libmvec.a") ;;
        -lgcc_s) args+=("-lgcc_eh") ;;
        -lc|-lpthread|-ldl|-lrt|-lutil|-lresolv) ;;
        *) args+=("$a") ;;
    esac
done
exec "$RUST_VBS_CC_STATIC" "${args[@]}" -Wl,-Bstatic
