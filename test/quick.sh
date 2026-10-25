#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# quick.sh -- the tiered regression set of every variant, one command, one results format.
#
#   test/quick.sh <variant> [--tier smoke|regression|extended] [--out DIR] [--vcpus N]
#                           [--list] [--dry-run] [--only STEP[,STEP...]]
#
#   variant   direct | vm | sev | sev-snp | sgx | vbs | tdx
#   tier      smoke       about 3 minutes: install check, helloworld, unit_tests examples, two LibOS tests (not on vbs, tdx)
#             regression  about 20 minutes at most (the default; includes smoke): PAL regression (direct, sgx),
#                         the LibOS core list (SAGUARO_TESTSET=core, test/quick/core-tests.txt), the examples
#                         bash sqlite python busybox memcached redis lighttpd rust nginx through their exec.sh, and the steps of
#                         the variant (see test/README.md). VM-based variants use ONE vCPU count: 2.
#             extended    the long suites (nightly or by hand): the full LibOS suite, the vCPU sweep (1 and 4),
#                         loops, stress tests (test/vm/stress), all examples, libos/test/fs, LTP
#   --vcpus   guest vCPUs of vm, sev and sev-snp (default 2; SAGUARO_CPU_NUM is set to it for every step,
#             except the sweep steps of the extended tier, whose names say their count)
#   --target riscv64   the direct variant for the RISC-V guest of scripts/riscv64/guest.sh: run it in the cross
#             container (container/run.sh --target riscv64) with the guest started; the tests run in the guest
#             through scripts/riscv64/run-suite.sh and run-examples.sh (the examples run as the suites of
#             test/direct/test-direct.sh there; QUICK_RISCV64_EXAMPLES names them, default: the examples of
#             the regression tier). The guest boot is not a step.
#   The rewrite stress (stress-rewrite, extended) runs on aarch64 and riscv64 only: x86_64 does not rewrite system calls.
#   --list    print the steps of the variant and tier, run nothing
#   --dry-run print the commands that would run
#   --only    run only the named steps (see --list)
#
# Output (OUT, default /tmp/saguaro-quick/<variant>-<arch>-<date>):
#   results.csv  variant,arch,commit,tier,step,test,result,seconds,vcpus,retried  (result: pass fail skip xfail xpass known-fail)
#   summary.txt  one line: QUICK variant=.. arch=.. tier=.. vcpus=.. commit=.. pass=N fail=N skip=N xfail=N xpass=N known=N retried=N secs=N status=PASS|FAIL
#   <step>.log   the raw output of each step (and <step>.xml, the JUnit file of a pytest step)
# Exit status 0 only if no test or step failed unexpectedly. A failure that expected-failures.toml lists for this
# variant, architecture and vCPU count is reported as known-fail and does not fail the run.
#
# Install prefixes (scripts/install.sh): direct $SAGUARO_DIRECT_PREFIX, vm/sev/sev-snp $SAGUARO_VM_PREFIX, sgx
# $SAGUARO_SGX_PREFIX; SAGUARO_PREFIX is used when the variant's own variable is not set. The VM variants must
# run as a user that may run saguaro-vm (root in the container of container/run.sh).
# Environment: QUICK_TIMEOUT_SCALE (multiplies every step timeout, default 1), QUICK_PYTEST_K (a pytest -k
# expression added to the LibOS steps, for a test that is too slow on a given host), SAGUARO_RAM_SIZE (default
# 8G for sev and sev-snp, the launcher of vm needs at least 5G), EXEC_TIMEOUT (seconds per example, exec.sh).
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=${REPO:-$(cd "$HERE/.." && pwd)}
Q=$HERE/quick
TARGET=""; VARIANT=""; TIER=regression; OUT=""; LIST=0; DRY=0; ONLY=""; CPUS=2
while [ $# -gt 0 ]; do
    case $1 in
        direct|vm|sev|sev-snp|sgx|vbs|tdx) VARIANT=$1 ;;
        --tier) TIER=${2:-}; shift ;;
        --out) OUT=${2:-}; shift ;;
        --vcpus) CPUS=${2:-}; shift ;;
        --target) TARGET=${2:-}; shift ;;
        --list) LIST=1 ;;
        --dry-run) DRY=1 ;;
        --only) ONLY=${2:-}; shift ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1 (test/quick.sh --help)" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$VARIANT" ] || { echo "usage: test/quick.sh <direct|vm|sev|sev-snp|sgx|vbs|tdx> [options]  (--help)" >&2; exit 2; }
case $TIER in smoke|regression|extended) ;; *) echo "bad tier: $TIER (smoke, regression or extended)" >&2; exit 2 ;; esac
case $CPUS in ''|*[!0-9]*|0) echo "bad --vcpus: $CPUS" >&2; exit 2 ;; esac

ARCH=$(uname -m)
case $TARGET in '') ;; riscv64) ARCH=riscv64 ;; *) echo "bad --target: $TARGET (riscv64)" >&2; exit 2 ;; esac
[ -z "$TARGET" ] || [ "$VARIANT" = direct ] || { echo "--target riscv64 is for the direct variant" >&2; exit 2; }
COMMIT=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
OUT=${OUT:-/tmp/saguaro-quick/$VARIANT-$ARCH-$(date +%Y%m%d-%H%M%S)}
SCALE=${QUICK_TIMEOUT_SCALE:-1}

# The launcher, the exec.sh flag, the install prefix and whether the variant has a vCPU count.
VCPUS=-
case $VARIANT in
    direct)  LAUNCH=saguaro-direct; EXEC=--direct;  PREFIX=${SAGUARO_DIRECT_PREFIX:-${SAGUARO_PREFIX:-$HOME/.local/saguaro}} ;;
    vm)      LAUNCH=saguaro-vm;     EXEC=--vm;      PREFIX=${SAGUARO_VM_PREFIX:-${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}}; VCPUS=$CPUS ;;
    sev)     LAUNCH=saguaro-sev;    EXEC=--sev;     PREFIX=${SAGUARO_VM_PREFIX:-${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}}; VCPUS=$CPUS ;;
    sev-snp) LAUNCH=saguaro-sev;    EXEC=--sev-snp; PREFIX=${SAGUARO_VM_PREFIX:-${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}}; VCPUS=$CPUS ;;
    sgx)     LAUNCH=saguaro-sgx;    EXEC=--sgx;     PREFIX=${SAGUARO_SGX_PREFIX:-${SAGUARO_PREFIX:-$HOME/.local/saguaro-sgx}} ;;
    vbs)     LAUNCH=saguaro-vbs;    EXEC=--vbs;     PREFIX= ;;
    tdx)     LAUNCH=;               EXEC=;          PREFIX= ;;
esac
VMLIKE=0; case $VARIANT in vm|sev|sev-snp) VMLIKE=1 ;; esac

# The environment of a real run (not of --list and --dry-run, which must work without an install).
setup_env() {
    [ -z "$TARGET" ] || PREFIX=
    if [ -n "$PREFIX" ]; then
        export SAGUARO_PREFIX=$PREFIX
        case $VARIANT in
            direct) export SAGUARO_DIRECT_PREFIX=$PREFIX ;;
            vm|sev|sev-snp) export SAGUARO_VM_PREFIX=$PREFIX ;;
            sgx) export SAGUARO_SGX_PREFIX=$PREFIX ;;
        esac
        if [ -r "$PREFIX/env.sh" ]; then . "$PREFIX/env.sh"; else . "$REPO/scripts/saguaro-env.sh"; fi
    fi
    if [ $VMLIKE = 1 ]; then
        # one vCPU count for the whole tier: the default of the launchers is overridden, no sweep
        export SAGUARO_CPU_NUM=$CPUS
        export SAGUARO_RAM_SIZE=${SAGUARO_RAM_SIZE:-8G}
    fi
    case $VARIANT in
        sev)     export SAGUARO_SEV_MODE=sev ;;
        sev-snp) export SAGUARO_SEV_MODE=snp ;;
    esac
    # the VBS examples need saguaro-vbs (~/.local/bin) and, for rust, cargo (~/.cargo/bin), which a non-login shell lacks
    [ "$VARIANT" != vbs ] || export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
    export EXEC_LOG_DIR=${EXEC_LOG_DIR:-$OUT/exec-logs}
}

# QEMU software emulation (TCG) is the accelerator of the aarch64 vm PAL when there is no usable /dev/kvm (or
# SAGUARO_ACCEL names another one); the same test as exec_accel in CI-Examples/common_tools/exec-common.sh
tcg() {
    [ "$VARIANT" = vm ] && [ "$ARCH" = aarch64 ] || return 1
    case ${SAGUARO_ACCEL:-} in
        kvm*) return 1 ;;
        ?*)   return 0 ;;
    esac
    ! { [ -r /dev/kvm ] && [ -w /dev/kvm ]; }
}

# Environment of the pytest suites: the launcher is chosen by the environment, as in the CI workflows.
libos_env() {
    case $VARIANT in
        vm)      echo "SAGUARO_VM=1" ;;
        sev|sev-snp) echo "SAGUARO_VM=1 SAGUARO_VM_LAUNCHER=saguaro-sev" ;;
        sgx)     echo "SGX=1" ;;
    esac
}
# $QUICK_JUNIT is the JUnit file of the step (set by the loop below); the string is expanded when the step runs
PYARGS='-q -rfEsxX --tb=short -p no:cacheprovider --junitxml=$QUICK_JUNIT -o junit_logging=no'
# the -k expression is quoted for the `bash -c` that runs the step (it has spaces and parentheses)
KSEL=; [ -z "${QUICK_PYTEST_K:-}" ] || KSEL=$(printf -- '-k %q' "not ($QUICK_PYTEST_K)")

# Steps: "name|tier|timeout-seconds|vcpus|kind|command"; tier is the lowest tier that runs the step
# (smoke < regression < extended); kind is pytest (a JUnit file gives one row per test), exec (the [PASS] and
# [FAIL] lines of exec.sh give one row per example) or plain (one row, the exit status decides).
# The commands run from the repository root with the environment of setup_env.
# The direct variant for the RISC-V guest (see --target). The tests run in the guest; the pytest suites print only
# the plain summary there, report.py reads it from the log.
steps_riscv64() {
    local run="scripts/riscv64/run-suite.sh" ex=${QUICK_RISCV64_EXAMPLES:-bash sqlite python busybox memcached redis lighttpd rust nginx}
    local t=${SAGUARO_TEST_TIMEOUT:-600}
    echo "install-check|smoke|60|-|plain|test -d build-riscv64/prefix && scripts/riscv64/guest.sh status"
    echo "check-core-list|smoke|60|-|plain|python3 test/quick/check-core-list.py"
    echo "ex-helloworld|smoke|1200|-|plain|scripts/riscv64/run-examples.sh helloworld"
    echo "ex-unit_tests|smoke|1200|-|plain|scripts/riscv64/run-examples.sh multithread networks"
    echo "libos-smoke|smoke|900|-|pytest|scripts/riscv64/render-test-manifests.sh libos >/dev/null && SAGUARO_TEST_TIMEOUT=$t $run libos -k 'test_202_fork_and_exec or test_600_multi_pthread'"
    [ "$TIER" = smoke ] && return
    echo "pal|regression|1800|-|pytest|scripts/riscv64/render-test-manifests.sh pal >/dev/null && SAGUARO_TEST_TIMEOUT=$t $run pal"
    echo "libos-core|regression|3600|-|pytest|scripts/riscv64/render-test-manifests.sh libos >/dev/null && SAGUARO_TESTSET=core SAGUARO_TEST_TIMEOUT=$t $run libos $KSEL"
    echo "ex-core|regression|3600|-|plain|scripts/riscv64/run-examples.sh $ex"
    [ "$TIER" = regression ] && return
    echo "libos-full|extended|7200|-|pytest|scripts/riscv64/render-test-manifests.sh libos >/dev/null && SAGUARO_TEST_TIMEOUT=$t $run libos $KSEL"
}

steps() {
    [ -z "$TARGET" ] || { steps_riscv64; return; }
    local env; env=$(libos_env)
    local cpu=$VCPUS
    local st="saguaro-test pytest --"

    if [ "$VARIANT" = tdx ]; then
        # the TDX PAL cannot run without TDX hardware (and the td-shim link step of the install is skipped):
        # the tier is the compile job, nothing else
        echo "tdx-build|smoke|1800|-|plain|[ -d build-tdx ] || meson setup build-tdx -Dtdx=enabled -Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dgraphics=disabled -Dlibc=none -Dtests=disabled -Dbuildtype=release; ninja -C build-tdx && test -x build-tdx/pal/src/host/tdx/pal"
        return
    fi
    if [ "$VARIANT" = vbs ]; then
        echo "install-check|smoke|60|-|plain|test -e build-vbs/runtime.stamp"
        echo "check-core-list|smoke|60|-|plain|python3 test/quick/check-core-list.py"
        echo "ex-helloworld|smoke|900|-|exec|(cd CI-Examples/helloworld && ./exec.sh $EXEC)"
        echo "ex-unit_tests|smoke|600|-|exec|(cd CI-Examples/unit_tests && ./exec.sh $EXEC)"   # every program: [SKIP] (no VBS build)
        [ "$TIER" = smoke ] && return
        local names; names=$(grep -v '^#' "$Q/vbs-core.txt" | cut -f2 | sort -u | paste -sd'|')
        echo "vbs-regress|regression|1500|-|regress|pal/src/host/vbs/test/regress/run-regress.sh \"\$QUICK_OUT/vbs-regress\" '^($names)(\\||\$)'"
        for e in busybox sqlite python redis lighttpd rust; do echo "ex-$e|regression|900|-|exec|(cd CI-Examples/$e && ./exec.sh $EXEC)"; done
        echo "ex-vbs|regression|600|-|exec|(cd CI-Examples/vbs && ./exec.sh $EXEC)"
        [ "$TIER" = regression ] && return
        echo "vbs-regress-full|extended|3600|-|plain|pal/src/host/vbs/test/regress/run-regress.sh \"\$QUICK_OUT/vbs-regress-full\""
        return
    fi

    # --- smoke ------------------------------------------------------------------------------------------------
    echo "install-check|smoke|60|$cpu|plain|command -v $LAUNCH saguaro-manifest saguaro-test && python3 -c 'import saguarolibos'"
    echo "check-core-list|smoke|60|-|plain|python3 test/quick/check-core-list.py"
    echo "ex-helloworld|smoke|300|$cpu|exec|(cd CI-Examples/helloworld && ./exec.sh $EXEC)"
    # thread-stress takes about 15 minutes under TCG: the extended tier runs it there (ex-thread-stress)
    local skip=; tcg && skip="UNIT_TESTS_SKIP=thread-stress "
    echo "ex-unit_tests|smoke|600|$cpu|exec|(cd CI-Examples/unit_tests && ${skip}./exec.sh $EXEC)"      # mremap, thread-tls/rwlock/stress, networks
    echo "libos-smoke|smoke|300|$cpu|pytest|(cd libos/test/regression && $env $st $PYARGS test_libos.py -k 'test_202_fork_and_exec or test_600_multi_pthread')"
    [ "$TIER" = smoke ] && return

    # --- regression (tier 1) ----------------------------------------------------------------------------------
    case $VARIANT in direct|sgx)
        echo "pal|regression|600|$cpu|pytest|(cd pal/regression && $env $st $PYARGS)" ;;      # 36 tests
    esac
    echo "libos-core|regression|1500|$cpu|pytest|(cd libos/test/regression && $env SAGUARO_TESTSET=core $st $PYARGS test_libos.py $KSEL)"
    for e in bash sqlite python busybox memcached redis lighttpd rust nginx; do
        echo "ex-$e|regression|600|$cpu|exec|(cd CI-Examples/$e && ./exec.sh $EXEC)"
    done
    case $VARIANT in
        direct)  echo "unit|regression|300|-|pytest|python3 -m pytest $PYARGS test/unit" ;;
        vm)      echo "vm-net|regression|600|$cpu|plain|test/vm/test-vm.sh --prefix \"\$SAGUARO_PREFIX\" loopback outbound"        # guest-loopback: extended (it hung on a hosted runner)
                 # the multi-process mode of the VM PAL (sys.experimental__multiprocess) is x86_64 only
                 [ "$ARCH" = x86_64 ] && echo "vm-multiproc|regression|900|$cpu|plain|test/vm/multiproc/check-probes.sh spawn signals forks misc sysv" ;;
        sev|sev-snp)
                 echo "sev-suite|regression|900|$cpu|plain|test/sev/test-sev.sh --prefix \"\$SAGUARO_PREFIX\" --vcpus $CPUS"
                 echo "vm-multiproc|regression|900|$cpu|plain|SAGUARO_VM_LAUNCHER=saguaro-sev test/vm/multiproc/check-probes.sh spawn signals forks misc sysv"
                 [ "$VARIANT" = sev-snp ] && echo "libos-snp-attest|regression|120|$cpu|pytest|(cd libos/test/regression && $env $st $PYARGS test_libos.py -k TC_04_AttestationSevSnp)" ;;
        sgx)     # TC_93_In_Out is not in the -k expression: it has not been run on SGX yet
                 # TC_92_avx runs only with AVX=1 (saguarolibos/regression.py); the sealed-file tests need tmp_enc/mrenclaves and tmp_enc/mrsigners
                 echo "libos-sgx-only|regression|900|-|pytest|(cd libos/test/regression && mkdir -p tmp_enc/mrenclaves tmp_enc/mrsigners && AVX=\$(grep -qw avx /proc/cpuinfo && echo 1 || echo 0) $env $st $PYARGS test_libos.py -k 'TC_03_FileCheckPolicy or TC_90_CpuidSGX or TC_91_RdtscSGX or TC_92_avx or sealed or mrenclave')"
                 echo "unit|regression|300|-|pytest|SGX=1 python3 -m pytest $PYARGS test/unit" ;;
    esac
    [ "$TIER" = regression ] && return

    # --- extended (tier 2): the long suites, the sweeps and the stress tests -----------------------------------
    case $VARIANT in
        direct|sgx)
            echo "libos-full|extended|3600|-|pytest|(cd libos/test/regression && $env $st $PYARGS test_libos.py $KSEL)" ;;
        vm|sev|sev-snp)
            echo "libos-full-${CPUS}vcpu|extended|3600|$cpu|pytest|(cd libos/test/regression && $env $st $PYARGS test_libos.py $KSEL)"
            for n in 1 4; do
                echo "libos-sweep-${n}vcpu|extended|3600|$n|pytest|(cd libos/test/regression && SAGUARO_CPU_NUM=$n $env $st $PYARGS test_libos.py $KSEL)"
            done ;;
    esac
    case $VARIANT in vm|sev|sev-snp)
        echo "unit|extended|300|-|pytest|python3 -m pytest $PYARGS test/unit" ;;
    esac
    tcg && echo "ex-thread-stress|extended|3600|$cpu|exec|(cd CI-Examples/unit_tests && EXEC_TIMEOUT=3000 UNIT_TESTS_ONLY=thread-stress ./exec.sh $EXEC)"
    echo "libos-fs|extended|900|$cpu|pytest|(cd libos/test/fs && $env $st $PYARGS test_fs.py test_tmpfs.py)"
    [ "$ARCH" = x86_64 ] && [ $VMLIKE = 0 ] && [ "$VARIANT" = direct ] \
        && echo "libos-abi|extended|300|-|pytest|(cd libos/test/abi/x86_64 && $st $PYARGS test_entrypoint.py)"
    local d n
    for d in "$REPO"/CI-Examples/*/exec.sh; do
        n=$(basename "$(dirname "$d")")
        case $n in helloworld|unit_tests|bash|sqlite|python|busybox|memcached|redis|lighttpd|rust|nginx) continue ;;
            graphics) continue ;;   # needs a display
            vbs) [ "$VARIANT" = vbs ] || continue ;; esac
        # an example that does not offer the variant is skipped, not failed (exec.sh without a backend lists them)
        echo "ex-$n|extended|900|$cpu|exec|(cd CI-Examples/$n && ./exec.sh 2>&1 | grep -q '^  ${EXEC#--}: yes' || { echo '[SKIP] $n: not offered by ${EXEC#--} on this host'; exit 0; }; ./exec.sh $EXEC)"
    done
    case $VARIANT in
        direct)  # x86_64 does not rewrite system calls, so the `blob` scenario fails there by design: this step runs on aarch64 and riscv64 only
                 [ "$ARCH" = x86_64 ] || echo "stress-rewrite|extended|1800|-|plain|scripts/stress_rewrite.sh -n 20"
                 echo "prefix-sweep|extended|1800|-|plain|test/direct/direct-prefix-sweep.sh" ;;
    esac
    case $VARIANT in vm|sev|sev-snp)
        local mp=; [ "$VARIANT" != vm ] && mp="SAGUARO_VM_LAUNCHER=saguaro-sev "
        [ "$VARIANT" = vm ] && echo "vm-net-all|extended|900|$cpu|plain|test/vm/test-vm.sh --prefix \"\$SAGUARO_PREFIX\" loopback epoll outbound udp guest-loopback"
        if [ "$ARCH" = x86_64 ]; then   # the multi-process mode of the VM PAL is x86_64 only
            echo "vm-multiproc-all|extended|1800|$cpu|plain|${mp}test/vm/multiproc/check-probes.sh spawn signals forks misc sysv libos examples redis"
            echo "vm-sig-loop|extended|3600|$cpu|plain|${mp}test/vm/multiproc/sig-loop.sh 100"
            echo "vm-boot-loop|extended|3600|$cpu|plain|${mp}test/vm/multiproc/boot-loop.sh 200"
        fi
        echo "vm-stress|extended|3600|$cpu|plain|${mp}test/vm/stress/run-stress.sh" ;;
    esac
    echo "ltp|extended|7200|$cpu|plain|[ -d libos/test/ltp/src ] || { echo 'LTP is not built in libos/test/ltp/src (libos/test/ltp/README.md): skipped'; exit 0; }; (cd libos/test/ltp && $env make regression)"
}

selected() {   # the steps of this variant and tier, honouring --only
    local name rest
    steps | while IFS='|' read -r name rest; do
        [ -z "$ONLY" ] || case ",$ONLY," in *",$name,"*) ;; *) continue ;; esac
        echo "$name|$rest"
    done
}

if [ $LIST = 1 ]; then
    printf '%-24s %-11s %-8s %-6s %s\n' STEP TIER TIMEOUT VCPUS COMMAND
    selected | while IFS='|' read -r n tr t v k c; do printf '%-24s %-11s %-8s %-6s %s\n' "$n" "$tr" "${t}s" "$v" "$c"; done
    exit 0
fi
if [ $DRY = 1 ]; then
    echo "# dry run: variant=$VARIANT arch=$ARCH tier=$TIER vcpus=$VCPUS prefix=${PREFIX:-none} out=$OUT"
    bad=0
    while IFS='|' read -r n tr t v k c; do
        # every command must parse as shell (a quoting bug in a step would fail only when the step runs)
        if bash -n -c "$c" 2> /dev/null; then ok=ok; else ok="SYNTAX ERROR"; bad=1; fi
        echo "DRY  $n (timeout $((t * SCALE))s, vcpus $v, $k, syntax $ok): $c"
    done < <(selected)
    exit $bad
fi

if [ -n "$ONLY" ]; then   # an unknown step name is an error, not a silent empty run
    for s in ${ONLY//,/ }; do
        steps | cut -d'|' -f1 | grep -qxF -- "$s" || { echo "unknown step for $VARIANT/$TIER: $s (--list)" >&2; exit 2; }
    done
fi
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd)
export QUICK_OUT=$OUT
setup_env
cd "$REPO" || exit 2
echo "variant,arch,commit,tier,step,test,result,seconds,vcpus,retried" > "$OUT/results.csv"
REP=("$Q/report.py")
COMMON=(--out "$OUT" --variant "$VARIANT" --arch "$ARCH" --commit "$COMMIT" --tier "$TIER")
T0=$SECONDS
echo "# quick.sh $VARIANT --tier $TIER: vcpus=$VCPUS commit=$COMMIT out=$OUT"
# A step that fails once is run again only for the helloworld example of sgx and vbs, and only when its log has
# the known allocation failure of the enclave (about 1 run in 8): the retry is printed and recorded (retried=1).
retriable() {
    case $VARIANT:$1 in sgx:ex-helloworld|vbs:ex-helloworld) grep -q 'mmap DATA_REGION' "$OUT/$1.log" ;; *) return 1 ;; esac
}
RETRIED=0
while IFS='|' read -r name tr tmo vc kind cmd; do
    s0=$(date +%s%N)
    retried=0
    export QUICK_JUNIT=$OUT/$name.xml
    while :; do
        rm -f "$QUICK_JUNIT"
        # timeout runs the step in its own process group and kills all of it at the limit
        timeout -s KILL $((tmo * SCALE)) bash -c "$cmd" > "$OUT/$name.log" 2>&1 < /dev/null
        rc=$?
        if [ $rc -ne 0 ] && [ $retried = 0 ] && retriable "$name"; then
            echo "NOTE retried: $name failed with 'mmap DATA_REGION' (a known flake of the enclave size); running it once more (first log: $name.first.log)"
            cp "$OUT/$name.log" "$OUT/$name.first.log"
            retried=1; RETRIED=$((RETRIED + 1))
            continue
        fi
        break
    done
    ms=$((($(date +%s%N) - s0) / 1000000)); dt=$(printf '%d.%02d' $((ms / 1000)) $((ms % 1000 / 10)))
    extra=()
    [ "$kind" = pytest ] && extra=(--junit "$QUICK_JUNIT" --summary-log "$OUT/$name.log")
    [ "$kind" = exec ] && extra=(--exec-log "$OUT/$name.log")
    [ "$kind" = regress ] && extra=(--regress-results "$OUT/$name/results.txt")
    python3 "${REP[0]}" add "${COMMON[@]}" --vcpus "$vc" --step "$name" --rc "$rc" --secs "$dt" --retried "$retried" "${extra[@]}"
    # a log above 5 MB (thread-stress prints a lot) is cut to its last 2000 lines, after the results were read from it
    if [ "$(stat -c %s "$OUT/$name.log")" -gt 5000000 ]; then
        { echo "[quick.sh: log cut to its last 2000 lines]"; tail -n 2000 "$OUT/$name.log"; } > "$OUT/$name.log.cut" \
            && mv "$OUT/$name.log.cut" "$OUT/$name.log"
    fi
done < <(selected)

python3 "${REP[0]}" summary "${COMMON[@]}" --vcpus "$VCPUS" --secs $((SECONDS - T0))
