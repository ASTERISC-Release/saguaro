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
#   EXAMPLE_EXPECT_FILE  file the program writes instead of printing: EXAMPLE_EXPECT is searched in it, not in
#                      the output (it is deleted before the run)
#   EXAMPLE_SKIP_UNSUPPORTED  1: a backend the example does not support is a SKIP, not a FAIL (the drivers over
#                      subdirectories set it)
#   EXAMPLE_PROGRAM    what the runner is given (default ./$EXAMPLE_BINARY; "java" for a manifest named java)
#   EXAMPLE_CHECK_TARGET  make target of mode "check" (default: check)
#   EXAMPLE_REQUIRES   programs the example needs on the host (checked with `command -v` before the run;
#                      a missing one is reported with the install-prereqs.sh that provides it)
#   EXAMPLE_REQUIRES_VBS  the same for the vbs backend, which builds the application from source with its own
#                      toolchain (default: none)
#   example_check_args  optional function: prints extra make variables for `make check` given the backend
#   EXAMPLE_NOTE       why it is skipped on this host when no backend applies (printed with the SKIP)
#
# Backends and where they are installed (scripts/install.sh; override with the env vars):
#   direct   $SAGUARO_DIRECT_PREFIX (default ~/.local/saguaro)      saguaro-direct
#   vm       $SAGUARO_VM_PREFIX     (default ~/.local/saguaro-vm)   saguaro-vm, needs /dev/kvm
#   sev      $SAGUARO_VM_PREFIX     (same install as vm, --sev)     saguaro-sev, x86_64 with SEV; plain SEV
#   sev-snp  (same install and runner as sev)                        saguaro-sev in SEV-SNP mode
#   sgx      $SAGUARO_SGX_PREFIX    (default ~/.local/saguaro-sgx)   saguaro-sgx, x86_64 with SGX (/dev/sgx_enclave);
#            scripts/install.sh --sgx; make SGX=1 signs the enclave with ~/.config/saguaro/enclave-key.pem
#   vbs      scripts/install.sh --vbs, in WSL on a Windows machine   Windows VBS enclave (make VBS=1); an
#            with VBS enclaves                                       example offers it when it has a
#                                                                    vbs.manifest.template
#
# Usage of exec.sh:
#   exec.sh --vm             build and run the example under backend vm; likewise --direct, --sev (plain
#                            SEV), --sev-snp (SEV-SNP), --sgx (`make SGX=1`: the signed enclave) and --vbs (`make VBS=1` and `make VBS=1 check`, in
#                            WSL; the runtime must be installed with scripts/install.sh --vbs first).
#   exec.sh --backend B      the same as --B (B is direct, vm, sev, sev-snp, sgx or vbs). Repeat either form for several
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
# Environment: EXEC_TIMEOUT (seconds per run, default 300; EXEC_BUILD_TIMEOUT, default 3600, for the build
# of a vbs image), EXEC_LOG_DIR (default /tmp/saguaro-exec),
# The backend sets SAGUARO_SEV_MODE for saguaro-sev: sev for --sev, snp for --sev-snp.

# EXEC_DIR (the directory to build and run in) and EXAMPLE_NAME default to the exec.sh's own directory;
# unit_tests/exec.sh sets them to run each test of its subdirectories
EXEC_DIR=${EXEC_DIR:-$(cd "$(dirname "$(readlink -f "$0")")" && pwd)}
EXAMPLE_NAME=${EXAMPLE_NAME:-$(basename "$EXEC_DIR")}
EXEC_LOG_DIR=${EXEC_LOG_DIR:-/tmp/saguaro-exec}
EXEC_TIMEOUT=${EXEC_TIMEOUT:-300}

exec_host_arch() { uname -m; }

# backend_base <backend>: the backend whose install and runner it uses (sev-snp is saguaro-sev, like sev)
backend_base() {
    case $1 in
        sev-snp) echo sev ;;
        *)       echo "$1" ;;
    esac
}

# backend_prefix <backend>
backend_prefix() {
    case $(backend_base "$1") in
        direct) echo "${SAGUARO_DIRECT_PREFIX:-$HOME/.local/saguaro}" ;;
        vm|sev) echo "${SAGUARO_VM_PREFIX:-$HOME/.local/saguaro-vm}" ;;
        sgx)    echo "${SAGUARO_SGX_PREFIX:-$HOME/.local/saguaro-sgx}" ;;
    esac
}

# backend_unavailable <backend>: prints why this host cannot run the backend and returns 0; returns
# 1 (and prints nothing) when it can.
backend_unavailable() {
    local b prefix runner
    b=$(backend_base "$1")
    if [ "$b" = vbs ]; then
        command -v powershell.exe >/dev/null \
            || { echo "VBS needs WSL on a Windows machine with VBS enclaves (no powershell.exe on this host)"; return 0; }
        local root
        root=$(git -C "$EXEC_DIR" rev-parse --show-toplevel 2>/dev/null)
        [ -e "$root/build-vbs/runtime.stamp" ] \
            || { echo "the VBS runtime is not installed in ${root:-this checkout} (scripts/install.sh --vbs)"; return 0; }
        return 1
    fi
    prefix=$(backend_prefix "$b")
    case $b in
        direct) runner=saguaro-direct ;;
        vm)     runner=saguaro-vm ;;
        sev)    runner=saguaro-sev ;;
        sgx)    runner=saguaro-sgx ;;
        *)      echo "unknown backend $b"; return 0 ;;
    esac
    if [ "$b" = sgx ]; then
        [ "$(exec_host_arch)" = x86_64 ] || { echo "SGX needs an x86_64 host"; return 0; }
        [ -e /dev/sgx_enclave ] || { echo "no SGX device (/dev/sgx_enclave): no SGX driver, or the container was created without it (container/run.sh)"; return 0; }
        [ -r /dev/sgx_enclave ] && [ -w /dev/sgx_enclave ] || { echo "no read/write access to /dev/sgx_enclave (group sgx)"; return 0; }
    fi
    if [ "$b" = sev ] && [ "$(exec_host_arch)" != x86_64 ]; then
        echo "SEV needs an x86_64 host"; return 0
    fi
    if [ "$b" = sev ]; then
        [ -e /dev/sev ] || { echo "no SEV device (/dev/sev) on this host"; return 0; }
        [ -r /dev/sev ] && [ -w /dev/sev ] || { echo "no read/write access to /dev/sev"; return 0; }
    fi
    # the aarch64 launcher falls back to software emulation (TCG) without /dev/kvm; x86_64 and SEV need KVM
    if [ "$b" = sev ] || { [ "$b" = vm ] && [ "$(uname -m)" != aarch64 ]; }; then
        [ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo "no read/write access to /dev/kvm"; return 0; }
    fi
    [ -x "$prefix/bin/$runner" ] || { echo "$runner not installed in $prefix (scripts/install.sh)"; return 0; }
    return 1
}

# exec_one_vbs: the example for a Windows VBS enclave: `make VBS=1` builds and signs the image (the
# runtime is installed by scripts/install.sh --vbs), `make VBS=1 check` runs the example's check against
# it. The check decides (EXAMPLE_EXPECT is not used: the check's echoed commands contain the text).
exec_one_vbs() {
    local b=$1 log out rc start verdict
    mkdir -p "$EXEC_LOG_DIR"
    log=$EXEC_LOG_DIR/$EXAMPLE_NAME-$b-$(exec_host_arch).log
    out=$EXEC_LOG_DIR/$EXAMPLE_NAME-$b-$(exec_host_arch).out
    start=$(date +%s)
    cd "$EXEC_DIR" || return 1
    { echo "== $EXAMPLE_NAME $b on $(exec_host_arch), $(date -Is), mode vbs"
      echo "+ make VBS=1"
      timeout -s KILL "${EXEC_BUILD_TIMEOUT:-3600}" make VBS=1 </dev/null; } 2>&1 | tee "$log"
    [ "${PIPESTATUS[0]}" = 0 ] \
        || { echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): build failed (see $log)"; return 1; }
    echo "+ make VBS=1 check" | tee -a "$log"
    timeout -s KILL "$EXEC_TIMEOUT" make VBS=1 check 2>&1 </dev/null | tee "$out"
    rc=${PIPESTATUS[0]}
    cat "$out" >>"$log"
    if [ $rc = 137 ]; then verdict="TIMEOUT"
    elif [ $rc != 0 ]; then verdict="FAIL(exit=$rc)"
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

# exec_accel <backend>: for the vm backend on aarch64, the accelerator the launcher will use: kvm, or tcg
# (QEMU software emulation: no usable /dev/kvm, or SAGUARO_ACCEL names tcg). Empty for everything else.
exec_accel() {
    [ "$(backend_base "$1")" = vm ] && [ "$(uname -m)" = aarch64 ] || return 0
    case ${SAGUARO_ACCEL:-} in
        kvm*) echo kvm ;;
        ?*)   echo tcg ;;
        *)    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then echo kvm; else echo tcg; fi ;;
    esac
}

# exec_one <backend>: build and run the example under one backend. Returns 0 PASS, 1 FAIL.
exec_one() {
    local b=$1 base prefix log out rc start verdict
    base=$(backend_base "$b")
    if [ "$base" = vbs ]; then exec_one_vbs "$b"; return; fi
    prefix=$(backend_prefix "$b")
    mkdir -p "$EXEC_LOG_DIR"
    log=$EXEC_LOG_DIR/$EXAMPLE_NAME-$b-$(exec_host_arch).log
    out=$EXEC_LOG_DIR/$EXAMPLE_NAME-$b-$(exec_host_arch).out
    start=$(date +%s)
    # only this shell sees the backend's saguaro-manifest and runner: source the install's env.sh
    # (scripts/install.sh writes it; it sets PATH and PYTHONPATH), or just put its bin first
    if [ -r "$prefix/env.sh" ]; then . "$prefix/env.sh"; else export PATH="$prefix/bin:$PATH"; fi
    # sgx: the Makefiles build the signed enclave manifest with SGX=1 (the target `all`, not only the manifest);
    # the enclave signing key is created once if it does not exist
    local mtarget=
    case $b in
        sev)     export SAGUARO_SEV_MODE=sev ;;
        sev-snp) export SAGUARO_SEV_MODE=snp ;;
        sgx)     export SGX=1; mtarget=all
                 # (a key at the earlier default ~/.config/gramine/ is still used by saguaro-sgx-sign)
                 [ -e "${SGX_SIGNER_KEY:-$HOME/.config/saguaro/enclave-key.pem}" ] \
                     || { [ -z "${SGX_SIGNER_KEY:-}" ] && [ -e "$HOME/.config/gramine/enclave-key.pem" ]; } \
                     || saguaro-sgx-gen-private-key ;;
    esac
    cd "$EXEC_DIR" || return 1
    # QEMU software emulation is much slower than KVM: say so (the log header repeats it)
    local accel accel_tag=
    accel=$(exec_accel "$b")
    if [ "$accel" = tcg ]; then
        accel_tag=", accelerator TCG (software emulation, no usable /dev/kvm)"
        echo "[note] $EXAMPLE_NAME $b on $(exec_host_arch): no usable /dev/kvm, QEMU runs under software emulation (TCG); expect slower runs"
    fi
    local binary=${EXAMPLE_BINARY:-$EXAMPLE_NAME}
    local manifest=${EXAMPLE_MANIFEST:-$binary.manifest}
    local runner mode=${EXAMPLE_MODE:-run}
    case $base in direct) runner=saguaro-direct ;; vm) runner=saguaro-vm ;; sev) runner=saguaro-sev ;; sgx) runner=saguaro-sgx ;; esac
    mtarget=${mtarget:-$manifest}

    if [ "$mode" = check ]; then
        # the example's own `make check`, with this backend as its GRAMINE
        { echo "== $EXAMPLE_NAME $b on $(exec_host_arch), $(date -Is), mode check$accel_tag"
          echo "+ rm -f $manifest; make $mtarget"
          rm -f "$manifest" "$manifest.sgx"; make "$mtarget"; } 2>&1 | tee "$log"
        [ "${PIPESTATUS[0]}" = 0 ] \
            || { echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): manifest failed (see $log)"; return 1; }
        # exec.sh may define example_check_args <backend>: extra make variables for `make check`
        local -a check_args=()
        if declare -F example_check_args >/dev/null; then check_args=($(example_check_args "$base")); fi
        echo "+ make ${EXAMPLE_CHECK_TARGET:-check} GRAMINE=$runner ${check_args[*]}" | tee -a "$log"
        timeout -s KILL "$EXEC_TIMEOUT" make "${EXAMPLE_CHECK_TARGET:-check}" GRAMINE="$runner" "${check_args[@]}" 2>&1 </dev/null | tee "$out"
        rc=${PIPESTATUS[0]}
        cat "$out" >>"$log"
    else
        # EXAMPLE_BUILD (set empty when make has no rule for the program) is the make target for the program
        local build=${EXAMPLE_BUILD-$binary}
        {
            echo "== $EXAMPLE_NAME $b on $(exec_host_arch), $(date -Is), mode run$accel_tag"
            if [ -n "$build" ]; then echo "+ make $build"; make "$build" || exit 1; fi
            # re-render the manifest for this backend (make -B would also rebuild the program)
            echo "+ rm -f $manifest; make $mtarget"
            rm -f "$manifest" "$manifest.sgx"; make "$mtarget"
        } 2>&1 | tee "$log"
        [ "${PIPESTATUS[0]}" = 0 ] \
            || { echo "[FAIL] $EXAMPLE_NAME $b $(exec_host_arch): build failed (see $log)"; return 1; }

        if [ "$(exec_host_arch)" = aarch64 ] && [ -e "$binary" ] && ! file -L "$binary" | grep -q "pie executable\|shared object"; then
            echo "[FAIL] $EXAMPLE_NAME $b aarch64: $binary is not PIE (the guest DRAM starts at 0x40000000)"
            return 1
        fi

        local program=${EXAMPLE_PROGRAM:-./$binary}
        [ -z "${EXAMPLE_EXPECT_FILE:-}" ] || rm -f "$EXAMPLE_EXPECT_FILE"
        echo "+ $runner $program ${EXAMPLE_ARGS[*]}" | tee -a "$log"
        timeout -s KILL "$EXEC_TIMEOUT" "$runner" "$program" "${EXAMPLE_ARGS[@]}" 2>&1 </dev/null | tee "$out"
        rc=${PIPESTATUS[0]}
        cat "$out" >>"$log"
    fi
    if [ $rc = 137 ]; then verdict="TIMEOUT"
    elif [ $rc != 0 ]; then verdict="FAIL(exit=$rc)"
    elif [ -n "${EXAMPLE_EXPECT:-}" ] && ! grep -qF -- "$EXAMPLE_EXPECT" "${EXAMPLE_EXPECT_FILE:-$out}"; then verdict="FAIL(output)"
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
            --backend) [ $# -ge 2 ] || { echo "--backend needs a value (direct, vm, sev, sev-snp, vbs or sgx)" >&2; exit 2; }
                       requested+=("$2"); shift 2 ;;
            --direct|--vm|--sev|--sev-snp|--vbs|--sgx) requested+=("${1#--}"); shift ;;
            --keep)    KEEP=1; shift ;;
            -h|--help) sed -n '2,/^[^#]/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
            *) echo "unknown option: $1" >&2; exit 2 ;;
        esac
    done

    local arch backend reason prog failed=0
    arch=$(exec_host_arch)
    if ! [[ " ${EXAMPLE_ARCHES:-x86_64 aarch64} " == *" $arch "* ]]; then
        echo "[SKIP] $EXAMPLE_NAME: host $arch is not in '${EXAMPLE_ARCHES:-x86_64 aarch64}'"
        exit 0
    fi

    # EXAMPLE_BACKENDS="" (set, empty) means the example has no backend here; only unset gets the default
    local supported=${EXAMPLE_BACKENDS-direct vm sev}
    # an example whose Makefile has an SGX=1 build (ifeq ($(SGX),1)) runs in an SGX enclave too
    if [ -n "$supported" ] && [ -f "$EXEC_DIR/Makefile" ] && grep -qF 'ifeq ($(SGX),1)' "$EXEC_DIR/Makefile" \
            && [[ " $supported " != *" sgx "* ]]; then
        supported="$supported sgx"
    fi
    # an example with a vbs.manifest.template runs for a Windows VBS enclave too (make VBS=1)
    if [ -f "$EXEC_DIR/vbs.manifest.template" ] && [[ " $supported " != *" vbs "* ]]; then
        supported="$supported vbs"
    fi
    local -a backends=()
    if [ ${#requested[@]} -eq 0 ]; then
        echo "usage: $0 --direct|--vm|--sev|--sev-snp|--vbs|--sgx [--keep]" >&2
        echo "$EXAMPLE_NAME supports these backends; this host ($arch) can run:" >&2
        for backend in ${supported/sev/sev sev-snp}; do
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
        if ! [[ " $supported " == *" $(backend_base "$backend") "* ]]; then
            if [ "${EXAMPLE_SKIP_UNSUPPORTED:-0}" = 1 ]; then
                echo "[SKIP] $EXAMPLE_NAME $backend: this test has no build for the backend"
                continue
            fi
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
        # the host programs of this backend: EXAMPLE_REQUIRES for direct, vm and sev; a VBS image is built from
        # source with its own toolchain (scripts/install.sh --vbs), so it has EXAMPLE_REQUIRES_VBS
        local missing= required=${EXAMPLE_REQUIRES:-}
        [ "$(backend_base "$backend")" != vbs ] || required=${EXAMPLE_REQUIRES_VBS:-}
        for prog in $required; do command -v "$prog" >/dev/null || missing+=" $prog"; done
        if [ -n "$missing" ]; then
            echo "[FAIL] $EXAMPLE_NAME $backend on $arch: cannot run here: missing on the host:$missing" \
                 "(run ./install-prereqs.sh in $EXEC_DIR)"
            failed=1
            continue
        fi
        exec_one "$backend" || failed=1
    done
    exit $failed
}
