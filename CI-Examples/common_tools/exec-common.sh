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
#   EXAMPLE_NOTE       why it is skipped on this host when no backend applies (printed with the SKIP)
#
# Backends and where they are installed (scripts/install.sh; override with the env vars):
#   direct   $GRAMINE_DIRECT_PREFIX (default ~/.local/gramine)      gramine-direct
#   vm       $GRAMINE_VM_PREFIX     (default ~/.local/gramine-vm)   gramine-vm, needs /dev/kvm
#   sev      $GRAMINE_VM_PREFIX     (same install as vm, --sev)     gramine-sev, x86_64 with SEV
#
# Usage of exec.sh:
#   exec.sh                  every backend the example supports that this host can run
#   exec.sh --backend B      only backend B (a missing backend is a FAIL, not a skip)
#   exec.sh --keep           keep the program output; logs are always written to $EXEC_LOG_DIR
#
# Exit status: 0 when nothing failed, 1 when a backend failed. A backend that this host cannot run
# is reported as SKIP with the reason and does not change the exit status.
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
          make -B "$manifest"; } >"$log" 2>&1 \
            || { echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): manifest failed (see $log)"; return 1; }
        timeout -s KILL "$EXEC_TIMEOUT" make check GRAMINE="$runner" >"$out" 2>&1 </dev/null
        rc=$?
        cat "$out" >>"$log"
    else
        # EXAMPLE_BUILD (set empty when make has no rule for the program) is the make target for the program
        local build=${EXAMPLE_BUILD-$binary}
        {
            echo "== $EXAMPLE_NAME $b on $(exec_host_arch), $(date -Is), mode run"
            if [ -n "$build" ]; then make "$build" || exit 1; fi
            make -B "$manifest"
        } >"$log" 2>&1 || { echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): build failed (see $log)"; return 1; }

        if [ "$(exec_host_arch)" = aarch64 ] && [ -e "$binary" ] && ! file -L "$binary" | grep -q "pie executable\|shared object"; then
            echo "[FAIL] $EXAMPLE_NAME $b aarch64: $binary is not PIE (the guest DRAM starts at 0x40000000)"
            return 1
        fi

        timeout -s KILL "$EXEC_TIMEOUT" "$runner" "./$binary" "${EXAMPLE_ARGS[@]}" >"$out" 2>>"$log" </dev/null
        rc=$?
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
            --backend) requested+=("$2"); shift 2 ;;
            --keep)    KEEP=1; shift ;;
            -h|--help) sed -n '2,20p' "$EXEC_DIR/../common_tools/exec-common.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
            *) echo "unknown option: $1" >&2; exit 2 ;;
        esac
    done

    local arch backend reason failed=0 ran=0
    arch=$(exec_host_arch)
    if ! [[ " ${EXAMPLE_ARCHES:-x86_64 aarch64} " == *" $arch "* ]]; then
        echo "[SKIP] $EXAMPLE_NAME: host $arch is not in '${EXAMPLE_ARCHES:-x86_64 aarch64}'"
        exit 0
    fi

    # EXAMPLE_BACKENDS="" (set, empty) means the example has no backend here; only unset gets the default
    local supported=${EXAMPLE_BACKENDS-direct vm sev}
    local -a backends=()
    if [ ${#requested[@]} -gt 0 ]; then
        backends=("${requested[@]}")
    else
        backends=($supported)
    fi

    for backend in "${backends[@]}"; do
        if ! [[ " $supported " == *" $backend "* ]]; then
            if [ ${#requested[@]} -gt 0 ]; then
                echo "[FAIL] $EXAMPLE_NAME $backend: not supported by this example"; failed=1
            else
                echo "[SKIP] $EXAMPLE_NAME $backend: not supported by this example"
            fi
            continue
        fi
        if reason=$(backend_unavailable "$backend"); then
            if [ ${#requested[@]} -gt 0 ]; then
                echo "[FAIL] $EXAMPLE_NAME $backend on $arch: $reason"; failed=1
            else
                echo "[SKIP] $EXAMPLE_NAME $backend on $arch: $reason"
            fi
            continue
        fi
        ran=$((ran + 1))
        exec_one "$backend" || failed=1
    done

    [ $ran -gt 0 ] || [ $failed = 1 ] || echo "[SKIP] $EXAMPLE_NAME: no backend to run on $arch${EXAMPLE_NOTE:+ ($EXAMPLE_NOTE)}"
    exit $failed
}
