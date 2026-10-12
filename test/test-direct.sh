#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# test-direct.sh — run the full aarch64 port test battery:
#   PAL regression, LibOS regression, and the CI example applications
#   (helloworld, multi-thread, busybox, bash, redis, memcached, sqlite, python,
#   lighttpd, nginx, rust, networks).
#
# Usage:
#   test/test-direct.sh                 # everything
#   test/test-direct.sh libos redis     # only the named suites
#   test/test-direct.sh --list          # list suite names
#   test/test-direct.sh --no-build      # skip manifest rebuilds of the pytest suites
#
# Environment overrides:
#   GRAMINE_PREFIX   install prefix (default: ~/.local/gramine)
#   LOGDIR           per-test log directory (default: ${TMPDIR:-/tmp}/gramine-multiarch-tests)
#   GRAMINE_RUNNER   gramine-direct (default) or gramine-vm: run the server examples (redis, memcached,
#                    lighttpd, nginx, rust) and `networks` under the VM PAL instead. The install must be
#                    a --vm build; each server's guest port is forwarded to the same port on 127.0.0.1
#                    by the VM's relay (GRAMINE_VM_INBOUND), so the clients are the ones of the direct run.
#                    nginx gets `master_process off;` (the VM PAL cannot create worker processes).
#                    Under the VM PAL the suites pal, libos and the exec/fork based ones do not apply.
#
# The script is architecture-neutral and also runs on x86_64 for parity
# testing, given a Gramine install built for that host (set GRAMINE_PREFIX to
# point at it). Example binaries/manifests are per-arch build artifacts; if an
# example binary is missing, foreign-arch, or its manifest embeds the other
# arch's triple, the script rebuilds it via that example's own Makefile.
# Expect different skip counts between arches (e.g. the x86-asm module is
# skipped on aarch64 and runs on x86_64; musl tests are the reverse), so
# parity means "0 failed" on both, not identical passed counts.
#
# Known cross-arch prerequisite quirk: busybox 1.32 needs CONFIG_TC,
# CONFIG_ARP and CONFIG_ARPING disabled in src/.config to compile against
# modern (>= ~6.x) kernel headers on any arch.
#
# ## Install steps (Debian/Ubuntu, arch-neutral — run from gramine-tdx/)
#
#   # 1. Build + runtime dependencies
#   sudo apt install build-essential meson ninja-build pkg-config \
#       python3-pytest python3-click python3-jinja2 python3-cryptography \
#       python3-voluptuous python3-toml nasm procps iproute2 file linux-libc-dev
#   (On this box voluptuous came from "pip3 install --user voluptuous" because
#    python3-voluptuous was not installed; either source works. click/jinja2/
#    cryptography resolve from apt here; elftools/_graminelibos_offsets are
#    only imported by the SGX-signing tools, which stay disabled.)
#
#   # 2. Configure, build, install into a private prefix (~3 min native build).
#   #    Or just run scripts/install.sh, which automates this step (and optionally
#   #    step 1 with --deps) and can chain into this script via --run-tests.
#   #    `ninja install` places the CLI scripts in <prefix>/bin and the
#   #    graminelibos package in <prefix>/lib/python3.X/site-packages — which
#   #    is why the script exports PYTHONPATH for exactly that directory.
#   #    (The vm/tdx options are Tirith forks of the upstream PAL; keep them
#   #    disabled for this direct-PAL test battery.)
#   meson setup build-$(uname -m) -Dbuildtype=release -Ddirect=enabled \
#       -Dsgx=disabled -Dvm=disabled -Dtdx=disabled -Dtests=enabled \
#       -Dlibc=glibc -Dprefix="$HOME/.local/gramine"
#   ninja -C build-$(uname -m)
#   ninja -C build-$(uname -m) install
#
#   # 3. Run the battery (GRAMINE_PREFIX/PYTHONPATH are set by the script
#   #    itself; export them manually only if you installed elsewhere):
#   test/test-direct.sh
#
#   # Iterating on a code change afterwards:
#   ninja -C build-$(uname -m) install && test/test-direct.sh --no-build
#
# nasm is only needed with -Dtests=enabled on x86_64 (the asm/x86_64 regression
# module); busybox/redis/memcached fetch their upstream tarballs on their
# example Makefiles' first build, so steps touching those suites need network.
#
# Every application runs under `timeout -s KILL`; a process-group kill plus a
# final sweep (pkill -9 -x loader / libpal.so) guarantee no strays are left
# behind after the script exits, including on Ctrl-C. (A trailing "Killed"
# job report from bash after server suites is the harness reaping its own
# server process — cosmetic, not a stray.)
#
# Lessons baked into this script (see Documentation/devel/aarch64/direct/FINDINGS.md):
#  * Example manifests pin loader.argv: passing extra app args (e.g. -vv)
#    makes the PAL exit the app instantly — so we always start servers with
#    zero arguments and talk to them over TCP instead.
#  * A killed TCP server leaves the port briefly busy; memcached's UDP
#    listener (same port) then logs "Address already in use" while its TCP
#    listener is fine. We wait for the port to be truly free and gate on
#    successful round-trips, never on log silence or fixed sleeps.
#  * Text-protocol clients must declare the exact value byte length or the
#    server correctly answers CLIENT_ERROR — the clients below are
#    byte-count-checked.

set -u

REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$REPO/scripts/gramine-env.sh"   # GRAMINE_PREFIX, PATH, PYTHONPATH, LOGDIR
mkdir -p "$LOGDIR"

ALL_SUITES=(pal libos helloworld multithread busybox bash redis memcached \
            sqlite python lighttpd nginx rust networks)
DO_BUILD=1
RUN=()

declare -a REQUESTED=()
for arg in "$@"; do
    case "$arg" in
        -h|--help)  awk '/^set -u/{exit} /^# test-direct.sh/{p=1} p&&/^#/{sub(/^# ?/,""); print}' "$0"; exit 0 ;;
        --list)     printf '%s\n' "${ALL_SUITES[@]}"; exit 0 ;;
        --no-build) DO_BUILD=0 ;;
        *)          matched=0
                    for s in "${ALL_SUITES[@]}"; do [ "$s" = "$arg" ] && { REQUESTED+=("$s"); matched=1; }; done
                    [ "$matched" = 1 ] || { echo "unknown suite: $arg (--list)"; exit 2; } ;;
    esac
done
if [ "${#REQUESTED[@]}" -gt 0 ]; then
    for s in "${ALL_SUITES[@]}"; do
        for r in "${REQUESTED[@]}"; do [ "$r" = "$s" ] && RUN+=("$s"); done
    done
    RUN=($(printf '%s\n' "${RUN[@]}" | sort -u))
else
    RUN=("${ALL_SUITES[@]}")
fi

RUNNER=${GRAMINE_RUNNER:-gramine-direct}
case "$RUNNER" in gramine-direct|gramine-vm) ;; *) echo "ERROR: GRAMINE_RUNNER must be gramine-direct or gramine-vm"; exit 1 ;; esac
for bin in "$RUNNER" gramine-test gramine-manifest; do
    command -v "$bin" >/dev/null || { echo "ERROR: $bin not on PATH (set GRAMINE_PREFIX?)"; exit 1; }
done
python3 -c 'import graminelibos' 2>/dev/null || { echo "ERROR: graminelibos not on PYTHONPATH"; exit 1; }

# --- architecture plumbing (script is arch-neutral: works on x86_64 too) ----
HOST_ARCH=$(uname -m)                    # "aarch64" | "x86_64" | "riscv64"
case "$HOST_ARCH" in
    aarch64) ELF_PAT="ARM aarch64"; FOREIGN_ARCH="x86_64" ;;
    x86_64)  ELF_PAT="x86-64";    FOREIGN_ARCH="aarch64" ;;
    riscv64) ELF_PAT="UCB RISC-V"; FOREIGN_ARCH="x86_64" ;;
    *)       ELF_PAT="";          FOREIGN_ARCH="" ;;
esac

# drop_stale_manifest <manifest>
# A manifest records the install it was rendered for. After the install prefix changed or went away
# (another prefix, a recreated container, a VM install used for the same example), or after the libc
# mode changed (a manifest rendered for a bundled glibc names runtime/glibc, which a libc=none install
# does not have), it has to be rendered again: the PAL otherwise fails with "Unable to load
# loader.entrypoint", cannot open the runtime directory, or loads another build's LibOS and crashes.
# If the manifest is stale in one of those ways, print why, delete the manifest and succeed;
# otherwise fail quietly.
drop_stale_manifest() {
    local libos rt
    libos=$(sed -n 's/^entrypoint = "file:\(.*libsysdb\.so\)"$/\1/p' "$1" 2>/dev/null | head -1)
    if [ -n "$libos" ] && { [ ! -f "$libos" ] || [ "${libos#"$GRAMINE_PREFIX"/}" = "$libos" ]; }; then
        rm -f "$1"
        echo "$libos"
        return 0
    fi
    # `file:<prefix>/lib/<multiarch>/gramine/runtime/glibc` in the manifest, but no such directory
    rt=$(grep -o 'file:[^" ]*/gramine/runtime/glibc' "$1" 2>/dev/null | head -1)
    if [ -n "$rt" ] && [ ! -d "${rt#file:}" ]; then
        rm -f "$1"
        echo "${rt#file:} (missing: this install has no bundled libc)"
        return 0
    fi
    return 1
}

# ensure_example <example-dir> <binary> <manifest>
# Build the example through its own Makefile if the binary is missing or was
# produced for another architecture, or if the manifest embeds the foreign
# arch triple (e.g. a tree generated on aarch64 now running on x86 and vice
# versa). `make` in the example dir is the sanctioned download+build pipeline.
ensure_example() {
    local dir=$REPO/CI-Examples/$1 bin=$2 man=$3 need=""
    local log=$LOGDIR/${1//\//-}-build.log   # "unit_tests/networks" -> "unit_tests-networks"
    if [ -n "$ELF_PAT" ] && ! file -b "$dir/$bin" 2>/dev/null | grep -q "$ELF_PAT"; then
        need="binary $1/$bin missing or not $HOST_ARCH"
    elif grep -q "$FOREIGN_ARCH" "$dir/$man" 2>/dev/null; then
        need="manifest $1/$man embeds $FOREIGN_ARCH paths"
    elif [ ! -s "$dir/$man" ]; then
        need="manifest $1/$man missing or empty"
    else
        local libos
        if libos=$(drop_stale_manifest "$dir/$man"); then
            need="manifest $1/$man was rendered for another Gramine install or libc mode ($libos)"
        fi
    fi
    if [ -n "$need" ]; then
        echo "  building example '$1' ($need; log: $log)"
        ( cd "$dir" && make >"$log" 2>&1 ) || {
            echo "  make failed for $1 — build it manually (see its README/Makefile)"
            tail -3 "$log" | sed 's/^/    /'
            return 1
        }
        # some example Makefiles stop before the final manifest step
        [ -s "$dir/$man" ] || ( cd "$dir" && make "${bin}.manifest" >>"$log" 2>&1 ) || true
    fi
    [ -x "$dir/$bin" ] && [ -s "$dir/$man" ]
}

# nginx_system_deps — true if the distro provides OpenSSL and PCRE/PCRE2 development files
nginx_system_deps() {
    command -v pkg-config >/dev/null && pkg-config --exists openssl \
        && { pkg-config --exists libpcre2-8 || pkg-config --exists libpcre; }
}

declare -a RESULTS=()
FAILED=0

kill_strays() {
    # A direct-PAL run appears in several shapes: the loader before it re-execs
    # (comm "loader"), after re-exec (comm "exe", cmdline ".../direct/loader ..."),
    # and forked host processes of fork()-using apps like nginx (comm "libpal.so",
    # cmdline ".../direct/libpal.so ..."). pkill -x demonstrably missed the latter
    # two families in battery runs, so match the Gramine-specific command lines
    # (bracketed so this script's own text can never match the patterns it runs).
    pkill -9 -x loader 2>/dev/null
    pkill -9 -f "/gramine/direct/[l]oader " 2>/dev/null
    pkill -9 -f "/gramine/direct/[l]ibpal[.]so " 2>/dev/null
    # gramine-vm: SIGKILL of the launcher leaves its QEMU running until the guest ends, and a server
    # guest never ends
    [ "$RUNNER" = gramine-vm ] && pkill -9 -f "[q]emu-system-.* -gramine-args init" 2>/dev/null
    sleep 0.3
}
trap kill_strays EXIT INT TERM

# summary <suite-name> <rc> [detail]
summary() {
    local detail=${3:-}
    if [ "$2" -eq 0 ]; then RESULTS+=("PASS  $1${detail:+ — $detail}")
    else                   RESULTS+=("FAIL  $1${detail:+ — $detail}"); FAILED=1; fi
}

# start_server <logname> <timeout-sec> <dir> <cmd...> — new session, PGID-killable
# guest ports of the server examples; under gramine-vm the relay forwards each one to the same port
# on 127.0.0.1 (the container), where the suites' clients connect
declare -A VM_INBOUND=([redis]="6379:6379" [memcached]="11211:11211" [lighttpd]="8003:8003"
                       [nginx]="8002:8002,8444:8444" [rust]="3000:3000")
start_server() {
    local name=$1 tmo=$2 dir=$3; shift 3
    local inbound=
    [ "$RUNNER" = gramine-vm ] && inbound=${VM_INBOUND[$name]:-}
    ( cd "$dir" && GRAMINE_VM_INBOUND="$inbound" setsid timeout -s KILL "$tmo" "$@" \
          >"$LOGDIR/$name.log" 2>&1 &
      echo $! >"$LOGDIR/$name.pid" )
}

stop_server() {
    local name=$1 pid i
    pid=$(cat "$LOGDIR/$name.pid" 2>/dev/null) || return 0
    kill -KILL -"$pid" 2>/dev/null
    for i in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.25; done
    wait "$pid" 2>/dev/null || true   # reap, and silence the "Killed" job report
    rm -f "$LOGDIR/$name.pid"
}

# wait_port_free <port> — no LISTEN socket (a TIME_WAIT is fine for our servers)
wait_port_free() {
    local port=$1 i
    for i in $(seq 1 40); do
        ss -tln "sport = :$port" 2>/dev/null | grep -q LISTEN || return 0
        sleep 0.5
    done
    return 1
}

# run_pytest <suite> <dir> — build manifests (unless --no-build) + pytest; prints count detail
run_pytest() {
    local suite=$1 dir=$2 detail
    if [ "$DO_BUILD" = 1 ]; then
        ( cd "$dir" && gramine-test build >"$LOGDIR/$suite-build.log" 2>&1 ) \
            || echo "  warning: manifest build failed; running pytest with existing manifests"
    fi
    ( cd "$dir" && timeout -s KILL 1500 python3 -m pytest -q >"$LOGDIR/$suite.log" 2>&1 )
    local rc=$?
    detail=$(grep -aoE '[0-9]+ (passed|failed|skipped|error[s]?)' "$LOGDIR/$suite.log" | tail -4 | tr '\n' ' ')
    echo "  [$detail] full log: $LOGDIR/$suite.log"
    return $rc
}

echo "=== Gramine $HOST_ARCH test battery — suites: ${RUN[*]} ==="

for suite in "${RUN[@]}"; do
echo
echo "--- $suite ---"
case "$suite" in

pal)
    run_pytest pal "$REPO/pal/regression"
    summary pal-regression $? "$(grep -aoE '[0-9]+ (passed|failed|skipped)' "$LOGDIR/pal.log" | tail -3 | tr '\n' ' ')"
    ;;

libos)
    run_pytest libos "$REPO/libos/test/regression"
    summary libos-regression $? "$(grep -aoE '[0-9]+ (passed|failed|skipped)' "$LOGDIR/libos.log" | tail -3 | tr '\n' ' ')"
    ;;

helloworld)
    if ! ensure_example helloworld helloworld helloworld.manifest; then summary helloworld 1 "build"; continue; fi
    timeout -s KILL 60 sh -c "cd '$REPO/CI-Examples/helloworld' && gramine-direct helloworld" \
        >"$LOGDIR/helloworld.log" 2>&1
    rc=$?
    grep -q "Hello, world" "$LOGDIR/helloworld.log" || rc=1
    [ $rc -ne 0 ] && tail -3 "$LOGDIR/helloworld.log"
    summary helloworld $rc
    ;;

multithread)
    rc=0
    for b in thread-tls thread-rwlock thread-stress; do
        if ! ensure_example unit_tests/multi-thread "$b" "$b.manifest"; then rc=1; break; fi
        if ! timeout -s KILL 120 sh -c "cd '$REPO/CI-Examples/unit_tests/multi-thread' && gramine-direct $b" \
                >"$LOGDIR/$b.log" 2>&1; then
            echo "  $b failed:"; tail -3 "$LOGDIR/$b.log"; rc=1
        fi
        kill_strays
    done
    summary multi-thread $rc "tls/rwlock/stress"
    ;;

busybox)
    if ! ensure_example busybox busybox busybox.manifest; then summary busybox 1 "build"; continue; fi
    timeout -s KILL 60 sh -c "cd '$REPO/CI-Examples/busybox' && gramine-direct busybox sh -c 'echo busybox-shell-works; uname -m'" \
        >"$LOGDIR/busybox.log" 2>&1
    rc=$?
    grep -q "busybox-shell-works" "$LOGDIR/busybox.log" || rc=1
    # the emulated shell must report the host's own machine (arch-neutral check)
    grep -qx "$HOST_ARCH" "$LOGDIR/busybox.log" || rc=1
    [ $rc -ne 0 ] && tail -3 "$LOGDIR/busybox.log"
    summary busybox $rc "sh + uname over emulated shell"
    ;;

bash)
    # The example ships its own 7-check regression target; it uses the system
    # bash (manifest mounts $(dirname $(which bash))), so no binary to build.
    ( cd "$REPO/CI-Examples/bash" && make clean >/dev/null 2>&1
      timeout -s KILL 300 make regression >"$LOGDIR/bash.log" 2>&1 )
    rc=$?
    n=$(grep -ac '^\[ Success [0-9]*/7 \]' "$LOGDIR/bash.log")
    [ "$n" -eq 7 ] || rc=1
    [ $rc -ne 0 ] && tail -3 "$LOGDIR/bash.log"
    kill_strays
    summary bash $rc "$n/7 checks"
    ;;

redis)
    if ! ensure_example redis redis-server redis-server.manifest; then summary redis 1 "build"; continue; fi
    wait_port_free 6379 || echo "  warning: port 6379 still busy"
    start_server redis 90 "$REPO/CI-Examples/redis" "$RUNNER" redis-server
    python3 - >"$LOGDIR/redis-client.log" 2>&1 <<'PYEOF'
import socket, time, sys
# Readiness = a full PING round-trip (not just connect(): both servers
# listen() early in startup). Note RESP replies carry their type byte:
# readline() yields b'+PONG' / b'+OK' / b'$2', value lines are raw.
f = None
deadline = time.time() + 60
while f is None:
    try:
        s = socket.create_connection(("127.0.0.1", 6379), timeout=2)
        cand = s.makefile("rwb")
        cand.write(b"PING\r\n"); cand.flush()
        if cand.readline().strip() == b"+PONG":
            f = cand
        else:
            cand.close(); s.close(); time.sleep(0.25)
    except OSError:
        time.sleep(0.25)
    if f is None and time.time() > deadline:
        sys.exit("redis never answered PING (see redis server log)")
def cmd(*lines):
    for l in lines: f.write((l + "\r\n").encode())
    f.flush(); return f.readline().strip()
# simple inline commands (the same form the readiness probe above uses)
assert cmd("PING") == b"+PONG", "PING"
assert cmd("SET k1 v1") == b"+OK", "SET"
assert cmd("GET k1") == b"$2", "GET header"
assert f.readline().strip() == b"v1", "GET value"
for i in range(20):
    val = f"val{i}"
    assert cmd(f"SET k{i} {val}") == b"+OK", i
    assert cmd(f"GET k{i}") == f"${len(val)}".encode(), i
    assert f.readline().strip() == val.encode(), i
print("redis: PING/SET/GET + 20 round-trips OK")
PYEOF
    rc=$?
    tail -2 "$LOGDIR/redis-client.log"
    stop_server redis
    kill_strays
    summary redis $rc "PING/SET/GET over TCP"
    ;;

memcached)
    if ! ensure_example memcached memcached memcached.manifest; then summary memcached 1 "build"; continue; fi
    wait_port_free 11211 || echo "  warning: port 11211 still busy"
    start_server memcached 120 "$REPO/CI-Examples/memcached" "$RUNNER" memcached
    python3 - >"$LOGDIR/memcached-client.log" 2>&1 <<'PYEOF'
import socket, time, sys
# Readiness = full "version" round-trip; see the redis comment above.
f = None
deadline = time.time() + 60
while f is None:
    try:
        s = socket.create_connection(("127.0.0.1", 11211), timeout=2)
        cand = s.makefile("rwb")
        cand.write(b"version\r\n"); cand.flush()
        if cand.readline().strip().startswith(b"VERSION"):
            f = cand
        else:
            cand.close(); s.close(); time.sleep(0.25)
    except OSError:
        time.sleep(0.25)
    if f is None and time.time() > deadline:
        sys.exit("memcached never answered 'version' (see server log)")
def cmd(*lines):
    for l in lines: f.write((l + "\r\n").encode())
    f.flush(); return f.readline().strip()
# counter semantics (value byte counts must be exact!)
# NB: cmd() takes whole protocol lines: the first args form the command line,
# a trailing arg is the value line.
assert cmd("set gcounter 0 60 1", "0") == b"STORED"
assert cmd("incr gcounter 1") == b"1"
assert cmd("incr gcounter 1") == b"2"
assert cmd("get gcounter") == b"VALUE gcounter 0 1"
assert f.readline().strip() == b"2" and f.readline().strip() == b"END"
for i in range(100):
    val = f"v{i:04d}"  # exactly 5 bytes, matching the declared bytes=5
    assert cmd(f"set k{i} 0 60 5", val) == b"STORED", i
    assert cmd(f"get k{i}") == b"VALUE k%d 0 5" % i, i
    assert f.readline().strip().decode() == val, i
    assert f.readline().strip() == b"END", i
assert cmd("delete k5") == b"DELETED"
assert cmd("get k5") == b"END"
print("memcached: counter semantics + 100 set/get round-trips OK")
PYEOF
    rc=$?
    tail -2 "$LOGDIR/memcached-client.log"
    stop_server memcached
    kill_strays
    summary memcached $rc "100 set/get over TCP"
    ;;

sqlite)
    # The example ships its own 3-check regression target (create/insert/select
    # scripts diffed against expected output); uses the host sqlite3 binary.
    ( cd "$REPO/CI-Examples/sqlite" && timeout -s KILL 300 make regression >"$LOGDIR/sqlite.log" 2>&1 )
    rc=$?
    n=$(grep -ac '^\[ Success [0-9]*/3 \]' "$LOGDIR/sqlite.log")
    [ "$n" -eq 3 ] || rc=1
    [ $rc -ne 0 ] && tail -3 "$LOGDIR/sqlite.log"
    kill_strays
    summary sqlite $rc "$n/3 checks"
    ;;

python)
    # run-tests.sh (via `make check`): helloworld, HTTP server + Gramine client +
    # host-wget round-trip equality on 8005, numpy dot, scipy cholesky. Uses the host
    # python3; numpy/scipy may be distro packages or a pip --user install (the manifest
    # re-adds the user site directory via loader.env.PYTHONPATH because the guest
    # interpreter runs as uid 0, which natively disables user site).
    if ! python3 -c "import numpy, scipy" 2>/dev/null; then
        summary python 0 "partial: numpy/scipy missing (apt python3-numpy python3-scipy or pip3 install --user --break-system-packages 'numpy scipy')"
        continue
    fi
    wait_port_free 8005 || echo "  warning: port 8005 still busy"
    # `make` does not render an existing manifest again, so one left by a run against another
    # install (for example the VM examples script) would make gramine-direct load that LibOS.
    if stale=$(drop_stale_manifest "$REPO/CI-Examples/python/python.manifest"); then
        echo "  python.manifest was rendered for another Gramine install or libc mode ($stale); rendering it again"
    fi
    ( cd "$REPO/CI-Examples/python" && make >/dev/null 2>&1
      timeout -s KILL 900 make check >"$LOGDIR/python.log" 2>&1 )
    rc=$?
    # `make check` redirects run-tests.sh into the example's TEST_STDOUT file
    n=$(grep -ac '^\[ Success [0-9]*/4 \]' "$REPO/CI-Examples/python/TEST_STDOUT" 2>/dev/null)
    [ "$n" -eq 4 ] || rc=1
    [ $rc -ne 0 ] && tail -3 "$LOGDIR/python.log"
    kill_strays
    summary python $rc "$n/4 checks"
    ;;

lighttpd)
    if ! ensure_example lighttpd install/sbin/lighttpd lighttpd.manifest; then
        summary lighttpd 1 "build (downloads lighttpd-1.4.59 via its Makefile)"; continue
    fi
    wait_port_free 8003 || echo "  warning: port 8003 still busy"
    start_server lighttpd 120 "$REPO/CI-Examples/lighttpd" "$RUNNER" lighttpd
    rc=1
    for i in $(seq 1 40); do
        if curl -sf --max-time 5 http://127.0.0.1:8003/random/10K.1.html \
            -o "$LOGDIR/lighttpd-fetch.html" 2>/dev/null; then
            cmp -s "$LOGDIR/lighttpd-fetch.html" \
                "$REPO/CI-Examples/lighttpd/install/html/random/10K.1.html" && rc=0
            break
        fi
        sleep 0.5
    done
    stop_server lighttpd
    kill_strays
    [ $rc -eq 0 ] && summary lighttpd 0 "10240B byte-identical over TCP" || summary lighttpd 1 "fetch/compare"
    ;;

nginx)
    # The example's Makefile needs the distro's OpenSSL and PCRE development files. Without them,
    # build with the bundled-sources fallback instead (nginx --with-openssl + a standalone PCRE).
    if ! nginx_system_deps \
       && ! file -b "$REPO/CI-Examples/nginx/install/sbin/nginx" 2>/dev/null | grep -q "$ELF_PAT"; then
        echo "  libssl/libpcre dev files not found; building nginx with bundled dependencies (log: $LOGDIR/nginx-build.log)"
        if ! ( cd "$REPO/CI-Examples/nginx" && ./build-with-bundled-deps.sh >"$LOGDIR/nginx-build.log" 2>&1 ); then
            tail -3 "$LOGDIR/nginx-build.log" | sed 's/^/    /'
            summary nginx 1 "bundled-deps build failed (needs curl, tar, autoconf/automake)"; continue
        fi
    fi
    if ! ensure_example nginx install/sbin/nginx nginx.manifest; then
        summary nginx 1 "build"; continue
    fi
    wait_port_free 8002 || echo "  warning: port 8002 still busy"
    if [ "$RUNNER" = gramine-vm ]; then
        # the VM PAL cannot create worker processes: run nginx as a single process
        grep -q '^master_process' "$REPO/CI-Examples/nginx/install/conf/nginx-gramine.conf" \
            || sed -i '1i master_process off;' "$REPO/CI-Examples/nginx/install/conf/nginx-gramine.conf"
    fi
    start_server nginx 120 "$REPO/CI-Examples/nginx" "$RUNNER" nginx
    rc=1
    for i in $(seq 1 40); do
        if curl -sf --max-time 5 http://127.0.0.1:8002/random/10K.1.html \
            -o "$LOGDIR/nginx-http.html" 2>/dev/null \
           && curl -sfk --max-time 5 https://127.0.0.1:8444/random/10K.1.html \
            -o "$LOGDIR/nginx-https.html" 2>/dev/null; then
            cmp -s "$LOGDIR/nginx-http.html"  "$REPO/CI-Examples/nginx/install/html/random/10K.1.html" \
              && cmp -s "$LOGDIR/nginx-https.html" "$REPO/CI-Examples/nginx/install/html/random/10K.1.html" \
              && rc=0
            break
        fi
        sleep 0.5
    done
    stop_server nginx
    kill_strays
    [ $rc -eq 0 ] && summary nginx 0 "HTTP+HTTPS 10240B byte-identical" || summary nginx 1 "fetch/compare"
    ;;

rust)
    if ! ensure_example rust target/release/rust-hyper-http-server rust-hyper-http-server.manifest; then
        summary rust 1 "build (needs cargo/rustc)"; continue
    fi
    wait_port_free 3000 || echo "  warning: port 3000 still busy"
    start_server rust 90 "$REPO/CI-Examples/rust" "$RUNNER" rust-hyper-http-server
    rc=1
    for i in $(seq 1 40); do
        if curl -sf --max-time 5 http://127.0.0.1:3000/ -o "$LOGDIR/rust-fetch.html" 2>/dev/null; then
            grep -q "Hello, World" "$LOGDIR/rust-fetch.html" && rc=0
            break
        fi
        sleep 0.5
    done
    stop_server rust
    kill_strays
    [ $rc -eq 0 ] && summary rust 0 "hyper HTTP 200 over TCP" || summary rust 1 "fetch/compare"
    ;;

networks)
    # Self-contained UDP self-test (server thread + loopback client thread): small/
    # medium/large datagrams, ordering, burst. The Makefile drops the graphics link
    # flags automatically when ../../../sharedgl/build is absent (see its comment).
    if ! ensure_example unit_tests/networks udp nw.manifest; then summary networks 1 "build"; continue; fi
    if timeout -s KILL 120 sh -c "cd '$REPO/CI-Examples/unit_tests/networks' && $RUNNER nw" \
            >"$LOGDIR/networks.log" 2>&1 \
       && grep -q "UDP SOCKET TEST PASSED" "$LOGDIR/networks.log"; then
        rc=0; detail=$(grep -ao "\[server\] OK .*" "$LOGDIR/networks.log" | head -1)
    else
        rc=1; detail=""; tail -3 "$LOGDIR/networks.log"
    fi
    kill_strays
    summary networks $rc "$detail"
    ;;

esac
done

echo
echo "================= SUMMARY ================="
printf '%s\n' "${RESULTS[@]}"
echo "==========================================="
echo "logs: $LOGDIR"
exit $FAILED
