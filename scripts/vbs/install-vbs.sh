#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `scripts/install.sh --vbs`: the WSL half of the VBS enclave tooling (dev docs: vbs-enclave/manual.md). Run it in
# the WSL2 Ubuntu distribution of the Windows machine, as a normal user (no sudo):
#
#   1. installs the missing build packages user-level (wsl-setup.sh user: Ubuntu packages unpacked
#      into ~/.local/share/saguaro-vbs/tools; nothing is installed system-wide);
#   2. puts `saguaro-vbs` on the PATH (a link in ~/.local/bin);
#   3. builds the vbs glibc, PAL and LibOS (once per checkout; `make VBS=1` does not build them and
#      stops when they are missing; run this again after changing the PAL or the LibOS);
#   4. checks the Windows side through powershell.exe and prints the Windows steps it cannot do.
#      It changes nothing on Windows.
#
# Idempotent: a second run finds everything in place. After it: `make VBS=1` in an example and
# `saguaro-vbs <program>`.
set -e

HERE=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
BIN=${SAGUARO_VBS_BIN:-$HOME/.local/bin}

if [ "$(id -u)" = 0 ]; then
    echo "ERROR: run install.sh --vbs as your normal user; it never needs root"
    exit 1
fi
if ! grep -qi microsoft /proc/version 2>/dev/null; then
    echo "note: this is not WSL; only the Linux half can be installed (SAGUARO_VBS_WINDOWS=user@host"
    echo "      names a Windows machine over ssh)."
fi

echo ">>> build packages (user-level, no sudo)"
if sh "$HERE/wsl-setup.sh" check clang >/dev/null 2>&1 || \
        { [ -f "${SAGUARO_VBS_TOOLS:-$HOME/.local/share/saguaro-vbs/tools}/env.sh" ] && \
          (. "${SAGUARO_VBS_TOOLS:-$HOME/.local/share/saguaro-vbs/tools}/env.sh"; \
           sh "$HERE/wsl-setup.sh" check clang >/dev/null 2>&1); }; then
    echo "all present"
else
    sh "$HERE/wsl-setup.sh" user clang
fi

echo ">>> saguaro-vbs on the PATH ($BIN)"
mkdir -p "$BIN"
ln -sfn "$HERE/saguaro-vbs" "$BIN/saguaro-vbs"
echo "$BIN/saguaro-vbs -> $HERE/saguaro-vbs"
case ":$PATH:" in
    *":$BIN:"*) ;;
    *) echo "note: $BIN is not in your PATH; add it:  export PATH=\"$BIN:\$PATH\"" ;;
esac

echo ">>> vbs glibc, PAL and LibOS (first time: several minutes)"
"$HERE/saguaro-vbs" runtime

echo ">>> Windows side"
if "$HERE/saguaro-vbs" check; then
    echo
    echo "Done. Next:  cd CI-Examples/helloworld && make VBS=1 && saguaro-vbs helloworld"
else
    cat <<'MSG'

The Windows side above is not ready. These steps change the machine and are done once on Windows
(install.sh does not do them); see Documentation/quickstart/vbs-enable.md, and
dev docs: vbs-enclave/windows-fresh-machine.md:
  - VBS running: Windows Security > Device security > Core isolation > Memory integrity, restart
  - Visual Studio (C++ desktop workload, build tools of this architecture) and the Windows SDK
  - the enclave test-signing certificate (CN=TheDefaultTestEnclaveCertName) and test signing on
Then run scripts/install.sh --vbs again (or `saguaro-vbs check`).
MSG
    exit 1
fi
