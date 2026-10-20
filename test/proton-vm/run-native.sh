#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Runs a Windows program through GE-Proton's `proton run` without a Steam client (the feasibility
# study, dev docs: proton-vm/): the compat data (Wine prefix) and an empty "Steam client
# install" are scratch folders under .proton-vm/. Usage: run-native.sh [-t trace.txt] [-p] EXE [ARGS]
# (-t: strace -f the run into the file; -p: use the proton script; the default calls the wine
# binary directly with the same environment the script would set up, after the prefix exists).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROTON=${PROTON:-$ROOT/.proton-vm/GE-Proton11-9-x86_64}
WORK=${WORK:-$ROOT/.proton-vm/work}
TRACE=; USE_SCRIPT=0
while getopts "t:p" o; do case $o in t) TRACE=$OPTARG ;; p) USE_SCRIPT=1 ;; esac; done
shift $((OPTIND - 1))
mkdir -p "$WORK/compatdata" "$WORK/steam"
export STEAM_COMPAT_DATA_PATH=$WORK/compatdata STEAM_COMPAT_CLIENT_INSTALL_PATH=$WORK/steam
export PROTON_LOG_DIR=$WORK
PRE=()
[ -z "$TRACE" ] || PRE=(strace -f -ttt -y -s 128 -o "$TRACE")
if [ "$USE_SCRIPT" = 1 ]; then
    exec "${PRE[@]}" python3 "$PROTON/proton" run "$@"
fi
# the environment `proton run` gives wine (from its log): prefix, paths, esync/fsync, DXVK overrides
export WINEPREFIX=$WORK/compatdata/pfx
export WINEDLLPATH=$PROTON/files/lib/wine/x86_64-unix:$PROTON/files/lib/wine/i386-unix
export PATH=$PROTON/files/bin:$PATH
export LD_LIBRARY_PATH=$PROTON/files/lib:$PROTON/files/lib64:${LD_LIBRARY_PATH:-}
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"d3d11,d3d10core,d3d9,dxgi,d3d12,d3d12core=n;steam.exe=b"}
export WINEESYNC=${WINEESYNC:-1} WINEFSYNC=${WINEFSYNC:-1} WINENTSYNC=${WINENTSYNC:-1}
export WINEDEBUG=${WINEDEBUG:--all}
exec "${PRE[@]}" "$PROTON/files/bin/wine" "$@"
