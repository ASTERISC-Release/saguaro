#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Multi-process checks for saguaro-vm with sys.experimental__multiprocess = true, with assertions.
# Each check runs under saguaro-vm, compares the output with the
# expected lines, and is rerun once before it counts as failed (hosted guests are slow and the
# signal probe has a known rare flake, see FINDINGS.md). Exit status 0 only if every check passed.
#
#   spawn     sh -c with a pipeline and command substitution; system(), posix_spawn (spawn.c)
#   signals   sig.sh under bash: a self signal, a signal to a child, exit statuses 5 and 143
#   forks     1000 sequential forks and 64 forks in batches of 8, every child reaped (forks.c)
#   misc      SysV IPC, memfd, file calls, timers and inotify used from a forked child (misc-fork.c)
#   sysv      System V shared memory, queues and semaphores between a parent and forked children,
#             IPC_RMID, SEM_UNDO, and a load of 2,000 messages from 4 children (sysv-fork.c)
#   libos     the LibOS regressions fork_and_exec and vfork_and_exec, manifests rendered with
#             SAGUARO_VM_MULTIPROCESS=1 (libos/test/regression/manifest.template)
#   examples  helloworld, thread-tls, python helloworld.py and the bash example's 7 checks, with the
#             key added to their generated manifests (restored afterwards)
#   redis     redis BGSAVE (a forked child writes dump.rdb); needs CI-Examples/redis built
#
# Usage: test/vm/multiproc/check-probes.sh [check...]   (default: spawn signals forks misc sysv)
# Environment: SAGUARO_PREFIX (install prefix, default ~/.local/saguaro-vm), SAGUARO_CPU_NUM
# (default 2), WORK (scratch dir, default <repo>/.mp-probes), LOGDIR (default $WORK/logs),
# TIMEOUT_SCALE (multiplies every timeout, default 1), RERUN (default 1: rerun a failed check once),
# MP (default 1; MP=0 renders the probe manifests without the key, as a negative check),
# REDIS_HOST_PORT (default 16379), SAGUARO_VM_LAUNCHER (default saguaro-vm; saguaro-sev for AMD SEV,
# applies to the checks that run through vm()).
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PREFIX=${SAGUARO_PREFIX:-$HOME/.local/saguaro-vm}
[ -f "$PREFIX/env.sh" ] || { echo "no $PREFIX/env.sh: set SAGUARO_PREFIX to the install prefix" >&2; exit 2; }
. "$PREFIX/env.sh"
command -v saguaro-vm >/dev/null || { echo "saguaro-vm not on PATH after $PREFIX/env.sh" >&2; exit 2; }
export SAGUARO_CPU_NUM=${SAGUARO_CPU_NUM:-2}
WORK=${WORK:-$REPO/.mp-probes}
LOGDIR=${LOGDIR:-$WORK/logs}
SCALE=${TIMEOUT_SCALE:-1}
RERUN=${RERUN:-1}
MP=${MP:-1}
ARCH_LIBDIR=/lib/$(gcc -dumpmachine)
mkdir -p "$WORK" "$LOGDIR"

FAILED=()
# vm <log> <timeout-seconds> <command...>: run a command (saguaro-vm ...) with a timeout, its
# stdout+stderr (carriage returns removed) appended to <log>. A guest still running at the timeout
# is looked at through its QEMU monitor (where its vCPUs are, vm-hang-report.py) and then stopped
# through it, so that the launcher still reports how the guest ended and its serial output.
vm() {
    local log=$1 t=$(( $2 * SCALE )) mon="$LOGDIR/qemu-monitor.sock" raw="$LOGDIR/vm-output.raw" pid rc i
    shift 2
    # SAGUARO_VM_LAUNCHER=saguaro-sev runs the checks under AMD SEV (dev docs: sev/)
    [ "$1" = saguaro-vm ] && [ -n "${SAGUARO_VM_LAUNCHER:-}" ] && set -- "$SAGUARO_VM_LAUNCHER" "${@:2}"
    echo "+ $* (timeout ${t}s, $SAGUARO_CPU_NUM vCPUs)" >> "$log"
    rm -f "$mon"
    SAGUARO_VM_MONITOR=$mon setsid "$@" > "$raw" 2>&1 < /dev/null &
    pid=$!
    for ((i = 0; i < t * 10; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then
        { echo "+ TIMEOUT after ${t}s"; "$HERE/vm-hang-report.py" "$mon" --quit; } > "$raw.hang" 2>&1
        for ((i = 0; i < 100; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -KILL -- "-$pid" 2>/dev/null   # the launcher's process group (setsid)
    fi
    wait "$pid"; rc=$?
    tr -d '\r' < "$raw" >> "$log"
    [ -f "$raw.hang" ] && cat "$raw.hang" >> "$log" && rm -f "$raw.hang"
    echo "+ exit status $rc" >> "$log"
}
# expect <log> <line>: the log has <line> as a whole line
expect() { grep -qxF -- "$2" "$1" || { echo "  missing line: '$2'"; return 1; }; }
# expect_sub <log> <text>: some line of the log contains <text>
expect_sub() { grep -qF -- "$2" "$1" || { echo "  missing text: '$2'"; return 1; }; }

# check <name>: run check_<name>, rerun once on failure, report PASS/FAIL with the log path
check() {
    local name=$1 log="$LOGDIR/$1.log" try
    for try in 1 2; do
        : > "$log"
        echo "=== $name (attempt $try)"
        if "check_$name" "$log"; then
            [ "$try" = 1 ] && echo "PASS $name" || echo "PASS $name (after a rerun; first attempt in $log.1)"
            # a child the LibOS marks disconnected (its parent then sees SIGPWR, "Power failure")
            # while the output is still right: reported, not failed (see FINDINGS.md)
            local d; d=$(grep -c 'got disconnected' "$log")
            [ "$d" = 0 ] || echo "WARN $name: $d 'Child process ... got disconnected' message(s) in $log"
            return 0
        fi
        echo "--- $name: output (filtered of VM launcher lines; full log $log)"
        grep -v -e '^Configuring Saguaro VM' -e '^REM_MEM_SIZE' -e '^\[boot\]' -e 'self-check' \
            -e '^(PAL) Physical' -e 'rame pool' -e '^vm-relay' "$log" | tail -n 40
        cp "$log" "$log.$try"
        [ "$RERUN" = 1 ] || break
    done
    echo "FAIL $name (log of each attempt: $log.<attempt>)"
    FAILED+=("$name")
    return 1
}

probes_built=0
build_probes() {
    [ "$probes_built" = 1 ] && return 0
    ( cd "$WORK" && cp "$HERE"/spawn.c "$HERE"/forks.c "$HERE"/misc-fork.c "$HERE"/sig.sh \
          "$HERE"/manifest.template "$HERE"/misc-fork.manifest.template "$HERE"/sysv-fork.c \
          "$HERE"/sysv-fork.manifest.template . \
      && gcc -O2 -o spawn spawn.c && gcc -O2 -o forks forks.c && gcc -O2 -o misc-fork misc-fork.c \
      && gcc -O2 -o sysv-fork sysv-fork.c ) \
        || { echo "probe build failed" >&2; exit 2; }
    local entry name e template
    for entry in /usr/bin/sh /usr/bin/bash "$WORK/spawn" "$WORK/forks" "$WORK/misc-fork" \
                 "$WORK/sysv-fork"; do
        name=$(basename "$entry")
        case $entry in "$WORK"/*) e=/probe/$name ;; *) e=$entry ;; esac
        template=manifest.template; [ "$name" = misc-fork ] && template=misc-fork.manifest.template
        [ "$name" = sysv-fork ] && template=sysv-fork.manifest.template
        ( cd "$WORK" && saguaro-manifest -Darch_libdir="$ARCH_LIBDIR" -Dentry="$e" -Dprobedir="$WORK" \
              "$template" "$name.manifest" ) 2>> "$LOGDIR/manifests.log" || { echo "manifest $name failed" >&2; exit 2; }
        [ "$MP" = 1 ] || sed -i '/^experimental__multiprocess = /d' "$WORK/$name.manifest"
    done
    mkdir -p "$WORK/scratch" "$WORK/tmp"
    probes_built=1
}

check_spawn() {
    local log=$1 rc=0
    build_probes
    mkdir -p "$WORK/countdir" && rm -f "$WORK"/countdir/* && touch "$WORK"/countdir/{a,b,c}
    # no double quotes in the arguments: saguaro-vm passes each argument to the guest inside
    # double quotes without escaping them (tools/saguaro-vm.in)
    ( cd "$WORK" && vm "$log" 120 saguaro-vm sh -c \
          'echo hi | cat; x=$(echo sub); echo subst=$x; echo count=$(ls /probe/countdir | wc -l)' )
    expect "$log" "hi" || rc=1
    expect "$log" "subst=sub" || rc=1
    expect "$log" "count=3" || rc=1
    ( cd "$WORK" && vm "$log" 120 saguaro-vm spawn )
    expect "$log" "system(true) = 0 (exit 0)" || rc=1
    expect "$log" "from posix_spawn" || rc=1
    expect "$log" "posix_spawn = 0, child exit 3" || rc=1
    # a missing pulseaudio gives exit 127; only the call returning matters here
    expect_sub "$log" "system(pulseaudio --check) = " || rc=1
    return $rc
}

check_signals() {
    local log=$1 rc=0 l
    build_probes
    ( cd "$WORK" && vm "$log" 120 saguaro-vm bash /probe/sig.sh )
    for l in "got USR1" "after-self" "child got USR2" "child-status=5" "term-status=143"; do
        expect "$log" "$l" || rc=1
    done
    return $rc
}

check_forks() {
    local log=$1 rc=0
    build_probes
    ( cd "$WORK" && vm "$log" 900 saguaro-vm forks 1000 1 )
    expect "$log" "forks=1000 parallel=1 bad=0" || rc=1
    ( cd "$WORK" && vm "$log" 600 saguaro-vm forks 64 8 )
    expect "$log" "forks=64 parallel=8 bad=0" || rc=1
    return $rc
}

check_misc() {
    local log=$1 rc=0
    build_probes
    rm -f "$WORK"/scratch/mf-* "$WORK"/tmp/.saguaro-*
    ( cd "$WORK" && vm "$log" 240 saguaro-vm misc-fork /probe/scratch )
    expect "$log" "misc-fork done" || rc=1
    local l
    for l in "sysv_shm (inherited attach shared both ways)" "sysv_msg (child's own queue)" \
             "memfd (child reads the parent's data)" \
             "files in a child (link, symlink, utimensat, renameat2, unlink)" \
             "timers in a child (timerfd, POSIX timer)" "inotify in a child"; do
        expect "$log" "$l: ok" || rc=1
    done
    if grep -q ': rc=' "$log"; then echo "  failed sub-checks:"; grep ': rc=' "$log" | sed 's/^/    /'; rc=1; fi
    return $rc
}

check_sysv() {
    local log=$1 rc=0
    build_probes
    ( cd "$WORK" && vm "$log" 240 saguaro-vm sysv-fork )
    expect "$log" "sysv-fork: 18/18 passed" || rc=1
    if grep -q '^FAIL \|^child step' "$log"; then grep '^FAIL \|^child step' "$log" | sed 's/^/  /'; rc=1; fi
    return $rc
}

check_libos() {
    local log=$1 rc=0 m
    (
        cd "$REPO/libos/test/regression" || exit 1
        # --force: saguaro-test leaves up-to-date manifests alone. The VM PAL runs several processes by
        # default, so the rendered manifests must not turn the key off.
        saguaro-test build --force >> "$log" 2>&1 || exit 1
        for m in fork_and_exec vfork_and_exec; do
            ! grep -qx 'experimental__multiprocess = false' "$m.manifest" \
                || { echo "  $m.manifest turns the key off" | tee -a "$log"; exit 1; }
        done
        SAGUARO_VM=1 SAGUARO_VM_SKIPLIST=0 timeout -s KILL $(( 900 * SCALE )) \
            python3 -m pytest -q -rfEsxX --tb=short -p no:cacheprovider test_libos.py \
            -k "vfork_and_exec or fork_and_exec" 2>&1 | tr -d '\r' >> "$log"
    ) || rc=1
    # render the manifests again without the key, so that later runs of the suite are unchanged
    ( cd "$REPO/libos/test/regression" && saguaro-test build --force > /dev/null 2>&1 ) \
        || echo "  warning: re-rendering the regression manifests without the key failed"
    tail -n 3 "$log" | sed 's/^/  /'
    grep -Eq '^[0-9]+ passed' "$log" && ! grep -Eq '[0-9]+ (failed|error)' "$log" || rc=1
    grep -Eq '^2 passed' "$log" || { echo "  expected '2 passed'"; rc=1; }
    return $rc
}

# key_on <manifest> <list>: add the key to a generated manifest, the original kept as <manifest>.mp-bak
key_on() {
    # the key is on by default: only a manifest that turns it off is patched (a second `experimental__multiprocess`
    # line would make the manifest invalid)
    grep -qx 'experimental__multiprocess = false' "$1" || return 0
    cp "$1" "$1.mp-bak" && echo "$1" >> "$2"
    sed -i 's/^experimental__multiprocess = false$/experimental__multiprocess = true/' "$1"
}
key_restore() { local m; while read -r m; do [ -f "$m.mp-bak" ] && mv "$m.mp-bak" "$m"; done < "$1"; : > "$1"; }

check_examples() {
    local log=$1 rc=0 E=$REPO/CI-Examples list="$WORK/patched" n
    : > "$list"
    ( cd "$E/helloworld" && make ) >> "$log" 2>&1 \
        && ( cd "$E/unit_tests/multi-thread" && make thread-tls thread-tls.manifest ) >> "$log" 2>&1 \
        && ( cd "$E/python" && make python.manifest ) >> "$log" 2>&1 \
        && ( cd "$E/bash" && make ) >> "$log" 2>&1 \
        || { echo "  example build failed"; return 1; }
    for m in "$E/helloworld/helloworld.manifest" "$E/unit_tests/multi-thread/thread-tls.manifest" \
             "$E/python/python.manifest" "$E/bash/bash.manifest"; do
        key_on "$m" "$list"
    done
    ( cd "$E/helloworld" && vm "$log" 180 saguaro-vm helloworld )
    expect_sub "$log" "Hello, world" || rc=1
    ( cd "$E/unit_tests/multi-thread" && vm "$log" 300 saguaro-vm thread-tls )
    expect_sub "$log" "(again)" || rc=1
    ( cd "$E/python" && vm "$log" 600 saguaro-vm python scripts/helloworld.py )
    expect_sub "$log" "Hello World" || rc=1
    # bash: the example's own checks; 2..7 start a second bash (a new process)
    ( cd "$E/bash" && rm -f OUTPUT && timeout -s KILL $(( 600 * SCALE )) make regression SAGUARO=saguaro-vm ) \
        2>&1 | tr -d '\r' >> "$log"
    n=$(grep -c '^\[ Success [0-9]*/7 \]' "$log")
    echo "  bash example: $n/7"
    [ "$n" = 7 ] || rc=1
    key_restore "$list"
    return $rc
}

check_redis() {
    local log=$1 rc=0 D=$REPO/CI-Examples/redis port=${REDIS_HOST_PORT:-16379} list="$WORK/patched" pid
    [ -f "$D/redis-server.manifest" ] || { echo "  CI-Examples/redis is not built"; return 1; }
    python3 -c "import socket,sys; s=socket.socket(); s.bind(('127.0.0.1', $port))" 2>/dev/null \
        || { echo "  host port $port is busy"; return 1; }
    : > "$list"; key_on "$D/redis-server.manifest" "$list"
    rm -f "$D/dump.rdb"
    ( cd "$D" && SAGUARO_VM_INBOUND="6379:$port" exec setsid timeout -s KILL $(( 300 * SCALE )) saguaro-vm redis-server \
          > "$log.server" 2>&1 ) &
    pid=$!
    python3 - "$port" "$SCALE" >> "$log" 2>&1 <<'PY' || rc=1
import socket, sys, time
port, scale = int(sys.argv[1]), int(sys.argv[2])
def cmd(s, *a):
    s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    time.sleep(0.2)
    return s.recv(65536).decode(errors="replace")
deadline = time.time() + 120 * scale
while True:
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        if "PONG" in cmd(s, "PING"):
            break
    except Exception:
        pass
    if time.time() > deadline:
        sys.exit("no PONG from redis")
    time.sleep(1)
for i in range(0, 1000, 100):
    cmd(s, "SET", "k%d" % i, "v%d" % i)
print("BGSAVE ->", cmd(s, "BGSAVE").strip())
for _ in range(int(120 * scale)):
    info = cmd(s, "INFO", "persistence")
    if "rdb_bgsave_in_progress:0" in info and "rdb_last_bgsave_status" in info:
        break
    time.sleep(1)
for l in info.splitlines():
    if l.startswith(("rdb_last_bgsave_status", "rdb_bgsave_in_progress")):
        print(l.strip())
print("GET k500 ->", cmd(s, "GET", "k500").strip().replace("\r\n", " "))
PY
    sleep 1
    kill -KILL -- -"$pid" 2>/dev/null; kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    key_restore "$list"
    tr -d '\r' < "$log.server" >> "$log"; rm -f "$log.server"
    expect "$log" "BGSAVE -> +Background saving started" || rc=1
    expect "$log" "rdb_last_bgsave_status:ok" || rc=1
    expect "$log" 'GET k500 -> $4 v500' || rc=1
    [ -s "$D/dump.rdb" ] && echo "  dump.rdb: $(stat -c %s "$D/dump.rdb") bytes" || { echo "  no dump.rdb"; rc=1; }
    rm -f "$D/dump.rdb"
    return $rc
}

[ $# -gt 0 ] || set -- spawn signals forks misc sysv
for c in "$@"; do
    case $c in
        spawn|signals|forks|misc|sysv|libos|examples|redis) check "$c" ;;
        *) echo "unknown check: $c" >&2; exit 2 ;;
    esac
done
if [ ${#FAILED[@]} = 0 ]; then
    echo "ALL MULTIPROCESS CHECKS PASSED ($*; MP=$MP, $SAGUARO_CPU_NUM vCPUs)"
else
    echo "MULTIPROCESS CHECKS FAILED: ${FAILED[*]} (MP=$MP, $SAGUARO_CPU_NUM vCPUs)"
    exit 1
fi
