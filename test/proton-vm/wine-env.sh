# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Source this (in the container) to run GE-Proton's 64-bit Wine directly, with the prefix that
# `proton run` created (test/proton-vm/run-native.sh -p). Then:
#   $WINE64 tests/hello.exe            # $WINE64 = wine-preloader + wine of files/lib/wine/x86_64-unix
# PROTON (the unpacked GE-Proton), WORK (the scratch folder) and PFX can be set before sourcing.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export PROTON=${PROTON:-$ROOT/.proton-vm/GE-Proton11-9-x86_64}
export WORK=${WORK:-$ROOT/.proton-vm/work}
export WINEPREFIX=${PFX:-$WORK/compatdata/pfx}
export WINEDLLPATH=$PROTON/files/lib/wine/x86_64-unix:$PROTON/files/lib/wine/i386-unix
export LD_LIBRARY_PATH=$PROTON/files/lib:$PROTON/files/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export PATH=$PROTON/files/bin:$PATH
export WINELOADERNOEXEC=1
export WINEESYNC=${WINEESYNC:-1} WINEFSYNC=${WINEFSYNC:-1} WINENTSYNC=${WINENTSYNC:-1}
export WINEDEBUG=${WINEDEBUG:--all}
# what `proton run` sets for DXVK and VKD3D-Proton (the DLLs are in the prefix's system32)
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"d3d11,d3d10core,d3d9,dxgi,d3d12,d3d12core=n"}
export WINE64="$PROTON/files/lib/wine/x86_64-unix/wine-preloader $PROTON/files/lib/wine/x86_64-unix/wine"
