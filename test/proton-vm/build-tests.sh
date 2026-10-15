#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
# Builds the Windows test programs of the Proton feasibility study with mingw-w64 (in the
# container: apt-get install -y gcc-mingw-w64-x86-64 gcc-mingw-w64-i686) into $OUT (default
# .proton-vm/tests at the repository root, outside git). hello.exe (64-bit), hello32.exe (32-bit),
# d3d11.exe (Direct3D 11 through DXVK) and vkwin.exe (Vulkan through winevulkan; the Vulkan headers
# come from the Linux loader package, libvulkan-dev).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../../.proton-vm/tests}
mkdir -p "$OUT"
x86_64-w64-mingw32-gcc -O2 -o "$OUT/hello.exe" "$HERE/hello.c"
i686-w64-mingw32-gcc -O2 -o "$OUT/hello32.exe" "$HERE/hello.c"
x86_64-w64-mingw32-gcc -O2 -o "$OUT/d3d11.exe" "$HERE/d3d11.c" -ld3d11 -ld3dcompiler -ldxgi -ldxguid -luuid
# only the Vulkan headers, not the rest of /usr/include (that would shadow mingw's libc headers)
mkdir -p "$OUT/include" && ln -sfn /usr/include/vulkan "$OUT/include/vulkan" && ln -sfn /usr/include/vk_video "$OUT/include/vk_video"
x86_64-w64-mingw32-gcc -O2 -I"$OUT/include" -o "$OUT/vkwin.exe" "$HERE/vkwin.c"
ls -l "$OUT"
