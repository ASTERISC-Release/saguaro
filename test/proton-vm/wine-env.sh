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
# GE-Proton's media path (mfmp4srcsnk.dll -> winedmo.so) needs FFmpeg 8 (libavformat.so.62), which
# Ubuntu 24.04 does not package: a minimal build in the container (Documentation/devel/proton-vm/FINDINGS.md,
# the game intake) sits in FFMPEG_LIBS; without it the intro videos of a Unity game never end
export FFMPEG_LIBS=${FFMPEG_LIBS:-/opt/ffmpeg8/lib}
[ -d "$FFMPEG_LIBS" ] && export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$FFMPEG_LIBS
export PATH=$PROTON/files/bin:$PATH
export WINELOADERNOEXEC=1
export WINEESYNC=${WINEESYNC:-1} WINEFSYNC=${WINEFSYNC:-1} WINENTSYNC=${WINENTSYNC:-1}
export WINEDEBUG=${WINEDEBUG:--all}
# what `proton run` sets for DXVK and VKD3D-Proton (the DLLs are in the prefix's system32)
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"d3d11,d3d10core,d3d9,dxgi,d3d12,d3d12core=n"}
export WINE64="$PROTON/files/lib/wine/x86_64-unix/wine-preloader $PROTON/files/lib/wine/x86_64-unix/wine"
# WOW64=1: a 32-bit game in Wine's new WoW64 mode (a 64-bit process running 32-bit code); the
# prefix is the phase-0 WoW64 prefix (PROTON_USE_WOW64=1). A null ALSA device so a game that
# refuses to start without a playback device (Psychonauts) finds one; a test convenience, silent.
# Under gramine-vm the guest's HOME is not read by ALSA: the container's /etc/asound.conf serves
# there (FINDINGS.md 2026-10-16, Psychonauts).
if [ "${WOW64:-0}" = 1 ]; then
    export WINEARCH=wow64
    export WINEPREFIX=${WOW64_PFX:-$ROOT/.proton-vm/work-wow64/compatdata/pfx}
    [ -f "$HOME/.asoundrc" ] || printf "pcm.!default { type null }
ctl.!default { type null }
" > "$HOME/.asoundrc"
fi
