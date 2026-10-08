#!/usr/bin/env bash
#
# run_all_tests.sh — run the full aarch64 port test battery:
#   PAL regression, LibOS regression, and the CI example applications
#   (helloworld, multi-thread, busybox, redis, memcached).
#
# Usage:
#   ./run_all_tests.sh                 # everything
#   ./run_all_tests.sh libos redis     # only the named suites
#   ./run_all_tests.sh --list          # list suite names
#   ./run_all_tests.sh --no-build      # skip manifest rebuilds of the pytest suites
#
# Environment overrides:
#   GRAMINE_PREFIX   install prefix (default: ~/.local/gramine)
#   LOGDIR           per-test log directory (default: $LOGDIR)
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
#   #    Or just run ./install.sh, which automates this step (and optionally
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
#   ./run_all_tests.sh
#
#   # Iterating on a code change afterwards:
#   ninja -C build-$(uname -m) install && ./run_all_tests.sh --no-build
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
# Lessons baked into this script (see ARM64_MILESTONE_A.md):
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

GRAMINE_PREFIX="${GRAMINE_PREFIX:-$HOME/.local/gramine}"
REPO="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
export PATH="$GRAMINE_PREFIX/bin:$PATH"
export PYTHONPATH="$GRAMINE_PREFIX/lib/python3.12/site-packages${PYTHONPATH:+:$PYTHONPATH}"
LOGDIR="${LOGDIR:-$LOGDIR}"
mkdir -p "$LOGDIR"

ALL_SUITES=(pal libos helloworld multithread busybox redis memcached)
DO_BUILD=1
RUN=()

declare -a REQUESTED=()
for arg in "$@"; do
    case "$arg" in
        -h|--help)  awk '/^set -u/{exit} /^#/{sub(/^# ?/,""); print}' "$0" | tail -n +2; exit 0 ;;
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

for bin in gramine-direct gramine-test gramine-manifest; do
    command -v "$bin" >/dev/null || { echo "ERROR: $bin not on PATH (set GRAMINE_PREFIX?)"; exit 1; }
done
python3 -c 'import graminelibos' 2>/dev/null || { echo "ERROR: graminelibos not on PYTHONPATH"; exit 1; }

# --- architecture plumbing (script is arch-neutral: works on x86_64 too) ----
HOST_ARCH=$(uname -m)                    # "aarch64" | "x86_64"
case "$HOST_ARCH" in
    aarch64) ELF_PAT="ARM aarch64"; FOREIGN_ARCH="x86_64" ;;
    x86_64)  ELF_PAT="x86-64";    FOREIGN_ARCH="aarch64" ;;
    *)       ELF_PAT="";          FOREIGN_ARCH="" ;;
esac

# ensure_example <example-dir> <binary> <manifest>
# Build the example through its own Makefile if the binary is missing or was
# produced for another architecture, or if the manifest embeds the foreign
# arch triple (e.g. a tree generated on aarch64 now running on x86 and vice
# versa). `make` in the example dir is the sanctioned download+build pipeline.
ensure_example() {
    local dir=$REPO/CI-Examples/$1 bin=$2 man=$3 need=""
    if [ -n "$ELF_PAT" ] && ! file -b "$dir/$bin" 2>/dev/null | grep -q "$ELF_PAT"; then
        need="binary $1/$bin missing or not $HOST_ARCH"
    elif grep -q "$FOREIGN_ARCH" "$dir/$man" 2>/dev/null; then
        need="manifest $1/$man embeds $FOREIGN_ARCH paths"
    elif [ ! -s "$dir/$man" ]; then
        need="manifest $1/$man missing or empty"
    fi
    if [ -n "$need" ]; then
        echo "  building example '$1' ($need; log: $LOGDIR/$1-build.log)"
        ( cd "$dir" && make >"$LOGDIR/$1-build.log" 2>&1 ) || {
            echo "  make failed for $1 — build it manually (see its README/Makefile)"
            tail -3 "$LOGDIR/$1-build.log" | sed 's/^/    /'
            return 1
        }
        # some example Makefiles stop before the final manifest step
        [ -s "$dir/$man" ] || ( cd "$dir" && make "${bin}.manifest" >>"$LOGDIR/$1-build.log" 2>&1 ) || true
    fi
    [ -x "$dir/$bin" ] && [ -s "$dir/$man" ]
}

declare -a RESULTS=()
FAILED=0

kill_strays() {
    pkill -9 -x loader 2>/dev/null
    pkill -9 -x libpal.so 2>/dev/null
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
start_server() {
    local name=$1 tmo=$2 dir=$3; shift 3
    ( cd "$dir" && setsid timeout -s KILL "$tmo" "$@" >"$LOGDIR/$name.log" 2>&1 &
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

echo "=== Gramine aarch64 test battery — suites: ${RUN[*]} ==="

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
        if ! ensure_example multi-thread "$b" "$b.manifest"; then rc=1; break; fi
        if ! timeout -s KILL 120 sh -c "cd '$REPO/CI-Examples/multi-thread' && gramine-direct $b" \
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

redis)
    if ! ensure_example redis redis-server redis-server.manifest; then summary redis 1 "build"; continue; fi
    wait_port_free 6379 || echo "  warning: port 6379 still busy"
    start_server redis 90 "$REPO/CI-Examples/redis" gramine-direct redis-server
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
    start_server memcached 120 "$REPO/CI-Examples/memcached" gramine-direct memcached
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

esac
done

echo
echo "================= SUMMARY ================="
printf '%s\n' "${RESULTS[@]}"
echo "==========================================="
echo "logs: $LOGDIR"
exit $FAILED
