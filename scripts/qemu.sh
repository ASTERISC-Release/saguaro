#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# qemu.sh -- fetch and build the QEMU backends pinned as git submodules under vmm/.
#
#   scripts/qemu.sh fetch <name>                              check out the pinned commit (shallow)
#   scripts/qemu.sh build <name> [--jobs N] [--no-deps] [--force]
#   scripts/qemu.sh path <name>                               print the path of the built binary
#   scripts/qemu.sh status                                    pinned commit, fetched, built
#
#   graphics   https://github.com/adilahmad17/qemu-graphics (branch dev-graphics): the host side of the
#              graphics redirection (KVM)
#   sev-tcg    https://github.com/adilahmad17/qemu-sev-tcg (branch dev-sev): the emulated SEV, SEV-ES and
#              SEV-SNP under TCG
#   tdx-tcg    https://github.com/adilahmad17/qemu-tdx-tcg (branch dev-tdx): the emulated TDX under TCG
#
# A clone of this repository does not contain the submodules: `fetch` runs
# `git submodule update --init --depth 1 -- vmm/<name>` (about 45 MB of git data, also when the pinned
# commit is not the tip of the fork's branch). `build` fetches the submodule when it is not checked out
# yet and runs the fork's build.sh, which installs bin/qemu-system-x86_64 and share/qemu into
# vmm/<name>/prefix (the binary finds its ROMs there, so use it from that directory). It installs the
# build packages first (apt-get, with sudo unless root; --no-deps skips this), and records the commit it
# built in vmm/<name>/prefix/.saguaro-commit: a second build of the same commit does nothing (--force
# rebuilds). For graphics the record also covers the graphics ABI files of this tree that the build
# compiles in. When the submodule is checked out at another commit than the pin (a pin being tested),
# that commit is built; `fetch` returns it to the pin. Needs git and network access (github.com, and
# gitlab.com for QEMU's bundled subprojects). Works on an Ubuntu or Debian host and in the container
# (container/run.sh), from any working directory.
#
# Use the result with QEMU_PATH, for example
#   SAGUARO_ACCEL=tcg QEMU_PATH=$(scripts/qemu.sh path sev-tcg) CI-Examples/helloworld/exec.sh --sev-snp
set -euo pipefail

ROOT=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)
NAMES="graphics sev-tcg tdx-tcg"
STAMP=.saguaro-commit

usage() { sed -n '5,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; }
die() { echo "qemu.sh: $*" >&2; exit 1; }

check_name() {
    case " $NAMES " in
        *" ${1:-} "*) ;;
        *) die "unknown backend '${1:-}' (known: $NAMES)" ;;
    esac
}

check_checkout() {
    command -v git >/dev/null || die "git is required"
    local top
    top=$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null) ||
        die "$ROOT is not a git checkout: the QEMU backends are git submodules, which a ZIP download or" \
            "a release tarball does not contain; clone the repository with git"
    [ "$(readlink -f "$top")" = "$(readlink -f "$ROOT")" ] || die "$ROOT is not the top of its git checkout"
}

# the commit of vmm/<name> recorded in HEAD of this repository
pinned() { git -C "$ROOT" rev-parse "HEAD:vmm/$1" 2>/dev/null || die "HEAD has no submodule vmm/$1"; }

# the commit checked out in vmm/<name>, or nothing when the submodule is not checked out
checked_out() {
    [ -e "$ROOT/vmm/$1/.git" ] || return 0
    git -C "$ROOT/vmm/$1" rev-parse HEAD 2>/dev/null || true
}

binary() { echo "$ROOT/vmm/$1/prefix/bin/qemu-system-x86_64"; }

# the stamp of a build: the commit of the fork and, for graphics, a hash of the files of this tree that
# its build compiles in (the graphics ABI and the shared xcb.c), so that a change of them rebuilds it
stamp_of() {
    local name=$1 head=$2
    if [ "$name" = graphics ]; then
        echo "$head $(cd "$ROOT" && cat libos/include/graphics_comm_abi.h libos/src/graphics/input_ring.h \
                      libos/src/graphics/xcb.c 2>/dev/null | sha256sum | cut -c1-16)"
    else
        echo "$head"
    fi
}

# the recorded stamp of the installed binary, if any
built_stamp() {
    local f=$ROOT/vmm/$1/prefix/$STAMP
    [ -x "$(binary "$1")" ] && [ -f "$f" ] && cat "$f"
    return 0
}

# the commit the installed binary was built from, if recorded
built_commit() { built_stamp "$1" | cut -d' ' -f1; }

do_fetch() {
    local name=$1
    local pin
    check_checkout
    pin=$(pinned "$name")
    echo ">>> fetching vmm/$name at ${pin:0:12}"
    # --depth 1 every time: the shallow setting of .gitmodules alone does not keep the clone shallow when
    # the pin is not the tip of the branch
    git -C "$ROOT" submodule update --init --depth 1 -- "vmm/$name"
}

do_build() {
    local name=$1; shift
    local jobs deps=1 force=0
    jobs=$(nproc)
    while [ $# -gt 0 ]; do
        case $1 in
            --jobs) jobs=${2:?--jobs needs a number}; shift ;;
            --no-deps) deps=0 ;;
            --force) force=1 ;;
            *) die "unknown argument for build: $1 (--help)" ;;
        esac
        shift
    done
    check_checkout
    local pin head
    pin=$(pinned "$name")
    head=$(checked_out "$name")
    if [ -z "$head" ]; then
        do_fetch "$name"
        head=$(checked_out "$name")
        [ -n "$head" ] || die "vmm/$name is not checked out after the fetch"
    elif [ "$head" != "$pin" ]; then
        echo "note: vmm/$name is at ${head:0:12}, not at the pinned ${pin:0:12}; building the checked-out" \
             "commit ('scripts/qemu.sh fetch $name' returns it to the pin)" >&2
    fi
    local dirty=
    [ -z "$(git -C "$ROOT/vmm/$name" status --porcelain --untracked-files=no)" ] || dirty=1
    local stamp
    stamp=$(stamp_of "$name" "$head")
    if [ $force = 0 ] && [ -z "$dirty" ] && [ "$(built_stamp "$name")" = "$stamp" ]; then
        echo "vmm/$name: ${head:0:12} is already built ($(binary "$name")); --force rebuilds"
        return 0
    fi
    [ -z "$dirty" ] || echo "note: vmm/$name has local changes; building them" >&2
    [ -x "$ROOT/vmm/$name/build.sh" ] || die "vmm/$name/build.sh is missing or not executable"
    local args=(--prefix "$ROOT/vmm/$name/prefix" --jobs "$jobs")
    [ $deps = 0 ] || args=(--install-deps "${args[@]}")
    rm -f "$ROOT/vmm/$name/prefix/$STAMP"
    echo ">>> building vmm/$name at ${head:0:12}"
    # the graphics fork resolves the graphics ABI from the Saguaro tree
    SAGUARO_ROOT=$ROOT "$ROOT/vmm/$name/build.sh" "${args[@]}"
    [ -x "$(binary "$name")" ] || die "the build finished without $(binary "$name")"
    [ -n "$dirty" ] || echo "$stamp" > "$ROOT/vmm/$name/prefix/$STAMP"
    echo ">>> vmm/$name built: QEMU_PATH=$(binary "$name")"
}

do_path() {
    local name=$1 b
    b=$(binary "$name")
    [ -x "$b" ] || die "vmm/$name is not built (no $b): run scripts/qemu.sh build $name"
    if [ -e "$ROOT/.git" ]; then
        local pin built
        pin=$(git -C "$ROOT" rev-parse "HEAD:vmm/$name" 2>/dev/null || true)
        built=$(built_commit "$name")
        [ -z "$pin" ] || [ "$built" = "$pin" ] ||
            echo "note: vmm/$name was built from ${built:-an unrecorded commit}, not from the pinned ${pin:0:12}" \
                 "(scripts/qemu.sh build $name)" >&2
    fi
    echo "$b"
}

do_status() {
    check_checkout
    local name pin head built fetched state
    printf '%-10s %-14s %-24s %s\n' backend pinned fetched built
    for name in $NAMES; do
        pin=$(pinned "$name")
        head=$(checked_out "$name")
        built=$(built_commit "$name")
        if [ -z "$head" ]; then fetched=no
        elif [ "$head" = "$pin" ]; then fetched=yes
        else fetched="yes (at ${head:0:12})"
        fi
        if [ -z "$built" ]; then
            if [ -x "$(binary "$name")" ]; then state="yes (unrecorded commit)"; else state=no; fi
        elif [ "$built" = "$pin" ]; then state=yes
        else state="yes (from ${built:0:12})"
        fi
        printf '%-10s %-14s %-24s %s\n' "$name" "${pin:0:12}" "$fetched" "$state"
    done
}

cmd=${1:-}
[ $# -eq 0 ] || shift
case $cmd in
    -h|--help|help) usage ;;
    fetch|build|path)
        [ $# -ge 1 ] || die "$cmd needs a backend name ($NAMES)"
        check_name "$1"
        [ "$cmd" = build ] || [ $# -eq 1 ] || die "$cmd takes one backend name"
        "do_$cmd" "$@" ;;
    status)
        [ $# -eq 0 ] || die "status takes no arguments"
        do_status ;;
    "") usage >&2; exit 2 ;;
    *) die "unknown command '$cmd' (fetch, build, path, status; --help)" ;;
esac
