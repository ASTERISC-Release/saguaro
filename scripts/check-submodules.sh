#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# check-submodules.sh -- check the QEMU submodules under vmm/ and print their pinned commits.
#
#   scripts/check-submodules.sh
#
# .gitmodules must have exactly the entries vmm/graphics, vmm/sev-tcg and vmm/tdx-tcg, each with its
# path, an https URL, a branch and shallow = true, and HEAD must have a gitlink (a pinned commit) for each.
# The submodules do not need to be checked out. Exits 1 with one line per problem.
set -euo pipefail

ROOT=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)
EXPECTED="vmm/graphics vmm/sev-tcg vmm/tdx-tcg"
problems=0
problem() { echo "check-submodules: $*" >&2; problems=$((problems + 1)); }

[ -f "$ROOT/.gitmodules" ] || { echo "check-submodules: no .gitmodules" >&2; exit 1; }
cfg() { git config -f "$ROOT/.gitmodules" "$@"; }

names=$(cfg --name-only --get-regexp '^submodule\..*\.path$' | sed 's/^submodule\.//; s/\.path$//' | sort | xargs)
[ "$names" = "$EXPECTED" ] || problem "the entries of .gitmodules are '$names', expected '$EXPECTED'"

for name in $EXPECTED; do
    path=$(cfg --get "submodule.$name.path" || true)
    url=$(cfg --get "submodule.$name.url" || true)
    branch=$(cfg --get "submodule.$name.branch" || true)
    shallow=$(cfg --type bool --get "submodule.$name.shallow" 2>/dev/null || true)
    [ "$path" = "$name" ] || problem "$name: path is '$path'"
    case $url in https://*) ;; *) problem "$name: url '$url' is not an https URL" ;; esac
    [ -n "$branch" ] || problem "$name: no branch"
    [ "$shallow" = true ] || problem "$name: shallow is not true"
    mode_sha=$(git -C "$ROOT" ls-tree HEAD -- "$name" | awk '{print $1, $3}')
    case $mode_sha in
        "160000 "*) printf '%-14s %s  %s %s\n' "$name" "${mode_sha#160000 }" "$branch" "$url" ;;
        *) problem "$name: HEAD has no gitlink at this path" ;;
    esac
done

# no other gitlink in HEAD
others=$(git -C "$ROOT" ls-tree -r HEAD | awk '$1 == "160000" {print $4}' | sort | xargs)
[ "$others" = "$EXPECTED" ] || problem "the gitlinks of HEAD are '$others', expected '$EXPECTED'"

[ $problems = 0 ] || exit 1
