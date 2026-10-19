#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# test-sev.sh -- regression tests of the AMD SEV build of the vm PAL (gramine-sev,
# Documentation/devel/sev/). Run it on an SEV host in the container of container/run.sh, with the
# QEMU of container/sev/build-sev-tools.sh (QEMU_PATH, default /root/sev-tools/prefix/bin/...).
#
#   hello    CI-Examples/helloworld under gramine-sev at 1 and 4 vCPUs; the guest must report
#            "SEV: memory encryption active" on its serial port
#   memory   test/sev/sev_probe: a virtiofs file read through the bounce buffers (checksum compared
#            with the host's), then a host-side look at guest memory through the QEMU monitor while
#            the probe holds a random marker: under SEV the marker and the PAL's ELF header must not
#            be visible anywhere in guest RAM, while the console text in the shared region must be;
#            the same run under gramine-vm (COMPARE=1, the default) must see all three
#   vsock    test/test-vm-loopback.sh under gramine-sev (host-initiated vsock connections into the
#            guest, through the shared region)
#   attest   SNP: test/sev/sev_attest gets an attestation report through /dev/attestation with
#            random report data; the host checks the report data, verifies the report against
#            AMD's VCEK chain (snpguest, certificates from AMD's KDS; SNP_CERTS caches them) and
#            compares its measurement with the digest gramine-sev-igvm.py computes (when known)
#   measure  plain SEV: a launch session from `sevctl session`, then the LAUNCH_MEASURE value that
#            QEMU reports (QMP query-sev-launch-measure) checked against the launch digest that
#            gramine-sev-igvm.py computes from the IGVM file, with the session's TIK
#
# Usage: test/test-sev.sh [--prefix DIR] [suite...]   (default: hello memory vsock)
#   GRAMINE_SEV_MODE=sev     the gramine-sev mode: sev or snp (measure: sev only)
#   GRAMINE_RAM_SIZE=8G      guest RAM (SEV pins all of it at launch, so smaller boots faster; the
#                            multiprocess frame pool, on by default, needs more than 4G)
#   COMPARE=1                memory: also run the probe under gramine-vm as the control
set -u
set -o pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PREFIX=${GRAMINE_PREFIX:-$HOME/.local/gramine-vm}
SUITES=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        hello|memory|vsock|measure|attest) SUITES+=("$1"); shift ;;
        -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ ${#SUITES[@]} -gt 0 ] || if [ "${GRAMINE_SEV_MODE:-sev}" = snp ]; then
    [ ${#SUITES[@]} -gt 0 ] || SUITES=(hello memory vsock attest)
else
    [ ${#SUITES[@]} -gt 0 ] || SUITES=(hello memory vsock measure)
fi

export PATH=$PREFIX/bin:$PATH
export PYTHONPATH=$(ls -d "$PREFIX"/lib/python3*/site-packages 2>/dev/null | head -1)
export QEMU_PATH=${QEMU_PATH:-/root/sev-tools/prefix/bin/qemu-system-x86_64}
export GRAMINE_RAM_SIZE=${GRAMINE_RAM_SIZE:-8G}
export GRAMINE_SEV_MODE=${GRAMINE_SEV_MODE:-sev}
RAM_GIB=${GRAMINE_RAM_SIZE%G}
OUT=${OUT:-/tmp/test-sev}
mkdir -p "$OUT"
command -v gramine-sev >/dev/null || { echo "gramine-sev not on PATH (prefix $PREFIX?)" >&2; exit 1; }

rc=0
fail() { echo "FAIL: $*"; rc=1; }
now() { date +%s.%N; }
elapsed() { python3 -c 'import sys; print(f"{float(sys.argv[2]) - float(sys.argv[1]):.2f}")' "$1" "$(now)"; }

test_hello() {
    cd "$REPO/CI-Examples/helloworld" || return
    make -s >/dev/null || { fail "building helloworld"; return; }
    for cpus in 1 4; do
        local t0 out serial=$OUT/hello-$cpus.serial
        t0=$(now)
        out=$(GRAMINE_CPU_NUM=$cpus GRAMINE_VM_SERIAL_LOG=$serial timeout 120 gramine-sev helloworld 2>&1)
        local r=$? t
        t=$(elapsed "$t0")
        if [ $r -ne 0 ] || ! grep -q "Hello, world" <<<"$out"; then
            fail "hello at $cpus vCPUs (exit $r): $(tail -n 5 <<<"$out")"
        elif ! grep -q "SEV: memory encryption active" "$serial"; then
            fail "hello at $cpus vCPUs: no 'SEV: memory encryption active' in $serial"
        else
            echo "hello: $cpus vCPUs, ${GRAMINE_RAM_SIZE}: ok in $t s ($(grep -o 'SEV: memory encryption active.*' "$serial"))"
        fi
    done
}

# probe_run <launcher> <expect: sev|plain>
probe_run() {
    local launcher=$1 expect=$2 tag=$1
    local mon=$OUT/$tag.monitor input=$OUT/$tag.input done=$OUT/$tag.done log=$OUT/$tag.out
    rm -f "$mon" "$done" "$log"
    head -c 300000 /dev/urandom > "$input"
    local host_sum
    host_sum=$(python3 -c '
import sys
s = 0
for b in open(sys.argv[1], "rb").read():
    s = (s * 31 + b) & 0xffffffffffffffff
print(f"len={len(open(sys.argv[1], "rb").read())} sum={s:016x}")' "$input")
    (cd "$HERE" && GRAMINE_VM_MONITOR=$mon GRAMINE_CPU_NUM=1 \
        timeout 300 "$launcher" sev_probe "$input" "$done" >"$log" 2>&1) &
    local pid=$!
    for _ in $(seq 600); do
        grep -q READY "$log" 2>/dev/null && break
        kill -0 $pid 2>/dev/null || break
        sleep 0.1
    done
    if ! grep -q READY "$log"; then
        fail "$tag: the probe did not get ready: $(tail -n 5 "$log")"
        touch "$done"; wait $pid; return
    fi
    if grep -q "INPUT $host_sum" "$log"; then
        echo "memory/$tag: virtiofs read of 300000 bytes ok ($host_sum)"
    else
        fail "$tag: virtiofs read: host $host_sum, guest $(grep INPUT "$log")"
    fi
    local addr xor
    addr=$(sed -n 's/.*SECRET addr=\(0x[0-9a-f]*\).*/\1/p' "$log")
    xor=$(sed -n 's/.*xor5a=\([0-9a-f]*\).*/\1/p' "$log")
    local res
    res=$(python3 "$HERE/sev_memcheck.py" --monitor "$mon" --addr "$addr" --xor5a "$xor" \
          --ram-gib "$RAM_GIB" 2>&1)
    echo "memory/$tag: $(tr '\n' ' ' <<<"$res")"
    touch "$done"
    wait $pid
    local r=$?
    grep -q "^DONE" "$log" && [ $r -eq 0 ] || fail "$tag: the probe exited with $r: $(tail -n 3 "$log")"
    local plain elf hits shared
    plain=$(sed -n 's/^secret_page_plain=//p' <<<"$res")
    elf=$(sed -n 's/^pal_page_elf=//p' <<<"$res")
    hits=$(sed -n 's/^marker_hits=//p' <<<"$res")
    shared=$(sed -n 's/^console_in_shared=//p' <<<"$res")
    if [ "$expect" = sev ]; then
        # SNP: QEMU keeps the plaintext copy of the (public, measured) launch image in the shared
        # backing of guest_memfd RAM, so the PAL's ELF header stays readable there; the private
        # pages themselves are not accessible to the host
        [ "$GRAMINE_SEV_MODE" = snp ] && elf=0
        [ "$plain" = 0 ] && [ "$elf" = 0 ] && [ "$hits" = 0 ] && [ "$shared" = 1 ] \
            || fail "$tag: the host sees private memory or not the shared region ($res)"
    else
        [ "$plain" = 1 ] && [ "$elf" = 1 ] && [ "${hits:-0}" -gt 0 ] && [ "$shared" = 1 ] \
            || fail "$tag: control run did not see guest memory ($res)"
    fi
}

test_memory() {
    make -C "$HERE" -s >/dev/null || { fail "building sev_probe"; return; }
    probe_run gramine-sev sev
    [ "${COMPARE:-1}" = 1 ] && probe_run gramine-vm plain
}

test_vsock() {
    GRAMINE_VM_LAUNCHER=gramine-sev SEQUENTIAL=${SEQUENTIAL:-20} CONCURRENT=${CONCURRENT:-4} \
        OUT=$OUT/vsock "$HERE/../vm/network/test-vm-loopback.sh" --prefix "$PREFIX" 2>&1 | tail -n 3 \
        || fail "vsock loopback under gramine-sev"
}

test_measure() {
    local sevctl=${SEVCTL:-/root/sev-tools/prefix/bin/sevctl}
    local igvm_tool
    igvm_tool=$(dirname "$(readlink -f "$(command -v gramine-sev)")")/../lib/*/gramine/sev/gramine-sev-igvm.py
    igvm_tool=$(ls $igvm_tool 2>/dev/null | head -1)
    local igvm=${igvm_tool%/*}/gramine-sev.igvm
    [ -x "$sevctl" ] || { fail "measure: no sevctl ($sevctl)"; return; }
    "$sevctl" export --full "$OUT/pdh.cert" >/dev/null || { fail "measure: sevctl export"; return; }
    (cd "$OUT" && rm -f gsev_* && "$sevctl" session --name gsev pdh.cert 1 >/dev/null) \
        || { fail "measure: sevctl session"; return; }
    make -C "$HERE" -s >/dev/null || { fail "building sev_probe"; return; }
    local qmp=$OUT/measure.qmp done=$OUT/measure.done log=$OUT/measure.out
    rm -f "$qmp" "$done"
    echo x > "$OUT/measure.input"
    (cd "$HERE" && GRAMINE_SEV_SESSION=$OUT/gsev GRAMINE_VM_QMP=$qmp GRAMINE_CPU_NUM=1 \
        timeout 300 gramine-sev sev_probe "$OUT/measure.input" "$done" >"$log" 2>&1) &
    local pid=$!
    for _ in $(seq 600); do
        grep -q READY "$log" 2>/dev/null && break
        kill -0 $pid 2>/dev/null || break
        sleep 0.1
    done
    local res
    res=$(python3 "$HERE/qmp.py" "$qmp" query-sev query-sev-launch-measure 2>&1)
    touch "$done"
    wait $pid || fail "measure: the probe failed: $(tail -n 3 "$log")"
    local info meas
    info=$(sed -n 1p <<<"$res")
    meas=$(sed -n 2p <<<"$res" | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"])')
    local args
    args=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d["api-major"], d["api-minor"], d["build-id"], d["policy"])' "$info") \
        || { fail "measure: query-sev: $res"; return; }
    read -r major minor build policy <<<"$args"
    echo "measure: query-sev $info"
    python3 "$igvm_tool" verify-sev "$igvm" --tik "$OUT/gsev_tik.bin" --measurement "$meas" \
        --api-major "$major" --api-minor "$minor" --build-id "$build" --policy "$policy" \
        | sed 's/^/measure: /' || fail "measure: the launch measurement does not match the IGVM file"
    local digest
    digest=$(python3 "$igvm_tool" measure "$igvm" | python3 -c 'import base64,sys; print(base64.b64encode(bytes.fromhex(sys.stdin.read().strip())).decode())')
    "$sevctl" measurement build --api-major "$major" --api-minor "$minor" --build-id "$build" \
        --policy "$policy" --tik "$OUT/gsev_tik.bin" --launch-measure-blob "$meas" \
        --launch-digest "$digest" 2>&1 | sed 's/^/measure: sevctl measurement build: /'
}

test_attest() {
    local snpguest=${SNPGUEST:-/root/sev-tools/prefix/bin/snpguest}
    make -C "$HERE" -s >/dev/null || { fail "building sev_attest"; return; }
    local data report=$OUT/attest-report.bin log=$OUT/attest.out
    data=$(head -c 64 /dev/urandom | od -An -tx1 | tr -d ' \n')
    rm -f "$report"
    (cd "$HERE" && GRAMINE_CPU_NUM=1 timeout 300 gramine-sev sev_attest "$data" "$report" \
        >"$log" 2>&1) || { fail "attest: the guest failed: $(tail -n 5 "$log")"; return; }
    grep -q "attestation_type=sev-snp" "$log" || fail "attest: attestation_type is not sev-snp"
    [ -s "$report" ] || { fail "attest: no report"; return; }
    echo "attest: $(grep -a 'quote size' "$log")"
    local got
    got=$(python3 -c 'import sys; print(open(sys.argv[1], "rb").read()[0x50:0x90].hex())' "$report")
    [ "$got" = "$data" ] && echo "attest: report data = the guest's user_report_data" \
        || fail "attest: report data $got, expected $data"
    local certs=${SNP_CERTS:-$OUT/certs}
    mkdir -p "$certs"
    if [ ! -s "$certs/ark.pem" ]; then
        "$snpguest" fetch ca pem "$certs" milan -e vcek >/dev/null || { fail "attest: fetch ca"; return; }
    fi
    "$snpguest" fetch vcek pem "$certs" "$report" >/dev/null || { fail "attest: fetch vcek"; return; }
    "$snpguest" verify certs "$certs" | sed 's/^/attest: /'
    "$snpguest" verify attestation "$certs" "$report" 2>&1 | sed 's/^/attest: /'
    "$snpguest" verify attestation "$certs" "$report" 2>&1 | grep -q "VEK signed the Attestation Report" \
        || fail "attest: the report does not verify"
    local meas
    meas=$(python3 -c 'import sys; print(open(sys.argv[1], "rb").read()[0x90:0xc0].hex())' "$report")
    echo "attest: measurement $meas"
    local igvm_tool igvm expected
    igvm_tool=$(ls "$(dirname "$(readlink -f "$(command -v gramine-sev)")")"/../lib/*/gramine/sev/gramine-sev-igvm.py | head -1)
    igvm=${igvm_tool%/*}/gramine-sev.igvm
    expected=$(python3 "$igvm_tool" measure --mode snp --vcpus 1 "$igvm" 2>/dev/null)
    if [ -n "$expected" ]; then
        [ "$expected" = "$meas" ] && echo "attest: measurement = gramine-sev-igvm.py measure --mode snp" \
            || fail "attest: measurement differs from the computed $expected"
    fi
    grep -q "launch digest of .* on 1 vCPUs: $meas" "$log" \
        && echo "attest: the digest gramine-sev printed at launch = the report's measurement" \
        || fail "attest: gramine-sev printed no launch digest equal to the report's"
    python3 "$igvm_tool" verify-snp "$igvm" "$report" --vcpus 1 --report-data "$data" \
        --certs "$certs" --snpguest "$snpguest" | sed 's/^/attest: verify-snp: /'
    [ "${PIPESTATUS[0]}" = 0 ] || fail "attest: verify-snp"
}

echo "test-sev: $(uptime)"
for s in "${SUITES[@]}"; do
    echo "=== $s"
    "test_$s"
done
[ $rc -eq 0 ] && echo "test-sev: PASS" || echo "test-sev: FAIL"
exit $rc
