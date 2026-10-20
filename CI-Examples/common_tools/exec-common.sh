# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Shared runner for the per-example `exec.sh` scripts (source this file from exec.sh; do not run it).
#
# Runs one example on the host it is invoked on, under the backends installed there. The host
# arch comes from `uname -m`; nothing is passed for it and nothing runs on another machine.
#
# The example's exec.sh sets these before sourcing this file and calling `exec_main "$@"`:
#   EXAMPLE_BINARY     program that `make` builds (default: the directory name)
#   EXAMPLE_MANIFEST   manifest target that `make` renders (default: $EXAMPLE_BINARY.manifest)
#   EXAMPLE_ARGS       array of arguments for the program (default: none)
#   EXAMPLE_EXPECT     substring the output must contain (default: only the exit status counts)
#   EXAMPLE_ARCHES     host arches it runs on (default: "x86_64 aarch64")
#   EXAMPLE_BACKENDS   backends it supports (default: "direct vm sev"; set it empty for none)
#   EXAMPLE_MODE       "run" (default): build the binary and run it with its args, and check the output
#                      "check": run the example's own `make check` with GRAMINE set to the backend
#   example_check_args  optional function: prints extra make variables for `make check` given the backend
#   EXAMPLE_NOTE       why it is skipped on this host when no backend applies (printed with the SKIP)
#
# Backends and where they are installed (scripts/install.sh; override with the env vars):
#   direct   $GRAMINE_DIRECT_PREFIX (default ~/.local/gramine)      gramine-direct
#   vm       $GRAMINE_VM_PREFIX     (default ~/.local/gramine-vm)   gramine-vm, needs /dev/kvm
#   sev      $GRAMINE_VM_PREFIX     (same install as vm, --sev)     gramine-sev, x86_64 with SEV
#
# Usage of exec.sh:
#   exec.sh --vm             build and run the example under backend vm; likewise --direct and --sev.
#   exec.sh --backend B      the same as --B (B is direct, vm or sev). Repeat either form for several
#                            backends. A backend is required: without one the script lists the
#                            backends and whether this host can run each, and exits with status 2.
#   exec.sh --keep           keep the program output; logs are always written to $EXEC_LOG_DIR
#
# The script prints each command it runs (lines starting with "+ "), including the command under
# test, and shows the build and program output on screen as it is produced (it is also written to the
# log in $EXEC_LOG_DIR).
#
# Exit status: 0 when nothing failed, 1 when a backend failed or cannot run here (the reason is
# printed), 2 for a usage error.
# Environment: EXEC_TIMEOUT (seconds per run, default 300), EXEC_LOG_DIR (default /tmp/gramine-exec),
# GRAMINE_SEV_MODE (sev or snp, default sev).

EXEC_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
EXAMPLE_NAME=$(basename "$EXEC_DIR")
EXEC_LOG_DIR=${EXEC_LOG_DIR:-/tmp/gramine-exec}
EXEC_TIMEOUT=${EXEC_TIMEOUT:-300}

exec_host_arch() { uname -m; }

# backend_prefix <backend>
backend_prefix() {
    case $1 in
        direct) echo "${GRAMINE_DIRECT_PREFIX:-$HOME/.local/gramine}" ;;
        vm|sev) echo "${GRAMINE_VM_PREFIX:-$HOME/.local/gramine-vm}" ;;
    esac
}

# backend_unavailable <backend>: prints why this host cannot run the backend and returns 0; returns
# 1 (and prints nothing) when it can.
backend_unavailable() {
    local b=$1 prefix runner
    prefix=$(backend_prefix "$b")
    case $b in
        direct) runner=gramine-direct ;;
        vm)     runner=gramine-vm ;;
        sev)    runner=gramine-sev ;;
        *)      echo "unknown backend $b"; return 0 ;;
    esac
    if [ "$b" = sev ] && [ "$(exec_host_arch)" != x86_64 ]; then
        echo "SEV needs an x86_64 host"; return 0
    fi
    if [ "$b" = sev ]; then
        [ -e /dev/sev ] || { echo "no SEV device (/dev/sev) on this host"; return 0; }
        [ -r /dev/sev ] && [ -w /dev/sev ] || { echo "no read/write access to /dev/sev"; return 0; }
    fi
    if [ "$b" = vm ] || [ "$b" = sev ]; then
        [ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo "no read/write access to /dev/kvm"; return 0; }
    fi
    [ -x "$prefix/bin/$runner" ] || { echo "$runner not installed in $prefix (scripts/install.sh)"; return 0; }
    return 1
}

# exec_one <backend>: build and run the example under one backend. Returns 0 PASS, 1 FAIL.
exec_one() {
    local b=$1 prefix log out rc start verdict
    prefix=$(backend_prefix "$b")
    mkdir -p "$EXEC_LOG_DIR"
    log=$EXEC_LOG_DIR/$EXAMPLE_NAME-$b-$(exec_host_arch).log
    out=$EXEC_LOG_DIR/$EXAMPLE_NAME-$b-$(exec_host_arch).out
    start=$(date +%s)
    # only this shell sees the backend's gramine-manifest and runner: source the install's env.sh
    # (scripts/install.sh writes it; it sets PATH and PYTHONPATH), or just put its bin first
    if [ -r "$prefix/env.sh" ]; then . "$prefix/env.sh"; else export PATH="$prefix/bin:$PATH"; fi
    export GRAMINE_SEV_MODE=${GRAMINE_SEV_MODE:-sev}
    cd "$EXEC_DIR" || return 1
    local binary=${EXAMPLE_BINARY:-$EXAMPLE_NAME}
    local manifest=${EXAMPLE_MANIFEST:-$binary.manifest}
    local runner mode=${EXAMPLE_MODE:-run}
    case $b in direct) runner=gramine-direct ;; vm) runner=gramine-vm ;; sev) runner=gramine-sev ;; esac

    if [ "$mode" = check ]; then
        # the example's own `make check`, with this backend as its GRAMINE
        { echo "== $EXAMPLE_NAME $b on $(exec_host_arch), $(date -Is), mode check"
          echo "+ make -B $manifest"
          make -B "$manifest"; } 2>&1 | tee "$log"
        [ "${PIPESTATUS[0]}" = 0 ] \
            || { echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): manifest failed (see $log)"; return 1; }
        # exec.sh may define example_check_args <backend>: extra make variables for `make check`
        local -a check_args=()
        if declare -F example_check_args >/dev/null; then check_args=($(example_check_args "$b")); fi
        echo "+ make check GRAMINE=$runner ${check_args[*]}" | tee -a "$log"
        timeout -s KILL "$EXEC_TIMEOUT" make check GRAMINE="$runner" "${check_args[@]}" 2>&1 </dev/null | tee "$out"
        rc=${PIPESTATUS[0]}
        cat "$out" >>"$log"
    else
        # EXAMPLE_BUILD (set empty when make has no rule for the program) is the make target for the program
        local build=${EXAMPLE_BUILD-$binary}
        {
            echo "== $EXAMPLE_NAME $b on $(exec_host_arch), $(date -Is), mode run"
            if [ -n "$build" ]; then echo "+ make $build"; make "$build" || exit 1; fi
            echo "+ make -B $manifest"
            make -B "$manifest"
        } 2>&1 | tee "$log"
        [ "${PIPESTATUS[0]}" = 0 ] \
            || { echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): build failed (see $log)"; return 1; }

        if [ "$(exec_host_arch)" = aarch64 ] && [ -e "$binary" ] && ! file -L "$binary" | grep -q "pie executable\|shared object"; then
            echo "[FAIL] $EXAMPLE_NAME $b aarch64: $binary is not PIE (the guest DRAM starts at 0x40000000)"
            return 1
        fi

        echo "+ $runner ./$binary ${EXAMPLE_ARGS[*]}" | tee -a "$log"
        timeout -s KILL "$EXEC_TIMEOUT" "$runner" "./$binary" "${EXAMPLE_ARGS[@]}" 2>&1 </dev/null | tee "$out"
        rc=${PIPESTATUS[0]}
        cat "$out" >>"$log"
    fi
    if [ $rc = 137 ]; then verdict="TIMEOUT"
    elif [ $rc != 0 ]; then verdict="FAIL(exit=$rc)"
    elif [ -n "${EXAMPLE_EXPECT:-}" ] && ! grep -qF -- "$EXAMPLE_EXPECT" "$out"; then verdict="FAIL(output)"
    else verdict=PASS
    fi
    local secs=$(( $(date +%s) - start ))
    if [ "$verdict" = PASS ]; then
        echo "[PASS] $EXAMPLE_NAME $b $(exec_host_arch) (${secs}s)"
        [ "${KEEP:-0}" = 1 ] || rm -f "$out"
        return 0
    fi
    echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): $verdict (log $log, output $out)"
    return 1
}

exec_main() {
    local -a requested=()
    KEEP=0
    while [ $# -gt 0 ]; do
        case $1 in
            --backend) [ $# -ge 2 ] || { echo "--backend needs a value (direct, vm or sev)" >&2; exit 2; }
                       requested+=("$2"); shift 2 ;;
            --direct|--vm|--sev) requested+=("${1#--}"); shift ;;
            --keep)    KEEP=1; shift ;;
            -h|--help) sed -n '2,/^[^#]/p' "$EXEC_DIR/../common_tools/exec-common.sh" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
            *) echo "unknown option: $1" >&2; exit 2 ;;
        esac
    done

    local arch backend reason failed=0
    arch=$(exec_host_arch)
    if ! [[ " ${EXAMPLE_ARCHES:-x86_64 aarch64} " == *" $arch "* ]]; then
        echo "[SKIP] $EXAMPLE_NAME: host $arch is not in '${EXAMPLE_ARCHES:-x86_64 aarch64}'"
        exit 0
    fi

    # EXAMPLE_BACKENDS="" (set, empty) means the example has no backend here; only unset gets the default
    local supported=${EXAMPLE_BACKENDS-direct vm sev}
    local -a backends=()
    if [ ${#requested[@]} -eq 0 ]; then
        echo "usage: $0 --direct|--vm|--sev [--keep]" >&2
        echo "$EXAMPLE_NAME supports these backends; this host ($arch) can run:" >&2
        for backend in $supported; do
            if reason=$(backend_unavailable "$backend"); then
                echo "  $backend: no, $reason" >&2
            else
                echo "  $backend: yes" >&2
            fi
        done
        [ -n "$supported" ] || echo "  (none${EXAMPLE_NOTE:+: $EXAMPLE_NOTE})" >&2
        exit 2
    fi
    backends=("${requested[@]}")

    for backend in "${backends[@]}"; do
        if ! [[ " $supported " == *" $backend "* ]]; then
            echo "[FAIL] $EXAMPLE_NAME $backend: this example does not support the backend" \
                 "(supported: ${supported:-none}${EXAMPLE_NOTE:+; $EXAMPLE_NOTE})"
            failed=1
            continue
        fi
        if reason=$(backend_unavailable "$backend"); then
            echo "[FAIL] $EXAMPLE_NAME $backend on $arch: cannot run here: $reason"
            failed=1
            continue
        fi
        exec_one "$backend" || failed=1
    done
    exit $failed
}
