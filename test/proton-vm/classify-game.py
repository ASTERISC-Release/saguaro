#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Classify an installed Windows game for the Proton-under-gramine-vm work
(dev docs: proton-vm/PLAN.md, "Next phases", step 0).

Walks a game folder, parses every .exe and .dll (PE headers only, no third-party module), and
prints: the machine type (i386 / x86-64 / ARM64) of each module, the subsystem of each .exe, the
Windows DLLs each module imports (import and delay-load tables), and a summary: bitness of the
game, the graphics / audio / input / launcher APIs it reaches through well-known DLLs, the GOG
info file's launch task (if any), and the redistributables the installer carries. Nothing from
the game is copied; the output is text for the matrix in TESTING.md.

Usage: classify-game.py [-v] GAME_DIR [GAME_DIR...]      (-v: every import of every module)
Run it with `python3 -I` on untrusted trees: it reads the files, it never executes them.
"""
import json
import os
import struct
import sys

MACHINES = {0x14C: "i386", 0x8664: "x86-64", 0xAA64: "ARM64", 0x1C0: "ARM", 0x200: "IA-64"}
SUBSYSTEMS = {1: "native", 2: "GUI", 3: "console", 9: "CE GUI"}

# DLL name (lower case, without .dll) -> what it tells us
KNOWN = {
    # graphics
    "d3d8": "graphics:Direct3D 8", "d3d9": "graphics:Direct3D 9", "d3d10": "graphics:Direct3D 10",
    "d3d10_1": "graphics:Direct3D 10.1", "d3d11": "graphics:Direct3D 11", "d3d12": "graphics:Direct3D 12",
    "dxgi": "graphics:DXGI", "ddraw": "graphics:DirectDraw", "opengl32": "graphics:OpenGL",
    "vulkan-1": "graphics:Vulkan", "glu32": "graphics:OpenGL (GLU)", "gdi32": "graphics:GDI",
    "d3dcompiler_47": "graphics:D3DCompiler 47", "d3dcompiler_43": "graphics:D3DCompiler 43",
    "d3dx9_43": "graphics:D3DX9", "dwmapi": "graphics:DWM", "dcomp": "graphics:DirectComposition",
    # audio
    "dsound": "audio:DirectSound", "xaudio2_7": "audio:XAudio2 2.7", "xaudio2_8": "audio:XAudio2 2.8",
    "xaudio2_9": "audio:XAudio2 2.9", "xaudio2_9redist": "audio:XAudio2 2.9 (redist)",
    "winmm": "audio:winmm", "mmdevapi": "audio:WASAPI (mmdevapi)", "openal32": "audio:OpenAL",
    "fmod": "audio:FMOD", "fmodex": "audio:FMOD Ex", "fmodstudio": "audio:FMOD Studio",
    "avrt": "audio:avrt", "msacm32": "audio:ACM", "mf": "media:Media Foundation",
    "mfplat": "media:Media Foundation", "mfreadwrite": "media:Media Foundation",
    "binkw32": "media:Bink video", "bink2w64": "media:Bink 2 video",
    # input
    "xinput1_3": "input:XInput 1.3", "xinput1_4": "input:XInput 1.4", "xinput9_1_0": "input:XInput 9.1",
    "dinput8": "input:DirectInput 8", "dinput": "input:DirectInput", "hid": "input:HID",
    "gameinput": "input:GameInput",
    # launchers, stores, DRM
    "steam_api": "launcher:Steam API", "steam_api64": "launcher:Steam API",
    "galaxy": "launcher:GOG Galaxy", "galaxy64": "launcher:GOG Galaxy",
    "eossdk-win64-shipping": "launcher:Epic Online Services", "eossdk-win32-shipping": "launcher:Epic Online Services",
    "xgamingruntime": "launcher:Xbox Gaming Runtime", "xgamingruntimethunks": "launcher:Xbox Gaming Runtime (thunks)",
    # runtimes
    "mscoree": "runtime:.NET Framework", "mono-2.0-bdwgc": "runtime:Mono",
    "msvcr100": "runtime:VC++ 2010 CRT", "msvcr110": "runtime:VC++ 2012 CRT", "msvcr120": "runtime:VC++ 2013 CRT",
    "msvcp100": "runtime:VC++ 2010 CRT", "msvcp110": "runtime:VC++ 2012 CRT", "msvcp120": "runtime:VC++ 2013 CRT",
    "msvcp140": "runtime:VC++ 2015+ CRT", "vcruntime140": "runtime:VC++ 2015+ CRT", "vcruntime140_1": "runtime:VC++ 2015+ CRT",
    "ucrtbase": "runtime:UCRT", "msvcrt": "runtime:msvcrt", "msvcr71": "runtime:VC++ 2003 CRT",
    "msvcr80": "runtime:VC++ 2005 CRT", "msvcr90": "runtime:VC++ 2008 CRT",
    "unityplayer": "engine:Unity", "gameassembly": "engine:Unity IL2CPP",
    "physxloader": "engine:PhysX", "physx3_x64": "engine:PhysX 3",
    # network
    "ws2_32": "network:Winsock", "wininet": "network:WinINet", "winhttp": "network:WinHTTP",
    "iphlpapi": "network:IP helper", "dnsapi": "network:DNS",
}
CATEGORY_ORDER = ["graphics", "audio", "media", "input", "launcher", "runtime", "engine", "network"]


def rva_to_off(sections, rva):
    for vaddr, vsize, raw, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rva - vaddr + raw
    return None


def read_cstr(data, off, limit=256):
    if off is None or off >= len(data):
        return None
    end = data.find(b"\0", off, off + limit)
    if end < 0:
        end = off + limit
    try:
        return data[off:end].decode("ascii")
    except UnicodeDecodeError:
        return None


def parse_pe(path):
    """Returns a dict with machine, bits, subsystem, imports (list of DLL names) or None."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    (pe_off,) = struct.unpack_from("<I", data, 0x3C)
    if pe_off + 24 > len(data) or data[pe_off:pe_off + 4] != b"PE\0\0":
        return None
    machine, nsect, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    opt = pe_off + 24
    if opt + 2 > len(data):
        return None
    (magic,) = struct.unpack_from("<H", data, opt)
    if magic == 0x10B:  # PE32
        bits, subsystem_off, ndirs_off, dirs_off = 32, opt + 68, opt + 92, opt + 96
    elif magic == 0x20B:  # PE32+
        bits, subsystem_off, ndirs_off, dirs_off = 64, opt + 68, opt + 108, opt + 112
    else:
        return None
    (subsystem,) = struct.unpack_from("<H", data, subsystem_off)
    (ndirs,) = struct.unpack_from("<I", data, ndirs_off)
    dirs = []
    for i in range(min(ndirs, 16)):
        dirs.append(struct.unpack_from("<II", data, dirs_off + 8 * i))
    sect_off = opt + opt_size
    sections = []
    for i in range(nsect):
        s = sect_off + 40 * i
        if s + 40 > len(data):
            break
        vsize, vaddr, rawsize, raw = struct.unpack_from("<IIII", data, s + 8)
        sections.append((vaddr, vsize, raw, rawsize))
    imports = []
    # import directory (1): 20-byte descriptors, name RVA at +12
    if len(dirs) > 1 and dirs[1][0]:
        off = rva_to_off(sections, dirs[1][0])
        while off is not None and off + 20 <= len(data):
            name_rva = struct.unpack_from("<I", data, off + 12)[0]
            if name_rva == 0:
                break
            name = read_cstr(data, rva_to_off(sections, name_rva))
            if name:
                imports.append(name)
            off += 20
    # delay-load directory (13): 32-byte descriptors, name RVA at +4
    if len(dirs) > 13 and dirs[13][0]:
        off = rva_to_off(sections, dirs[13][0])
        while off is not None and off + 32 <= len(data):
            name_rva = struct.unpack_from("<I", data, off + 4)[0]
            if name_rva == 0:
                break
            name = read_cstr(data, rva_to_off(sections, name_rva))
            if name:
                imports.append(name + " (delay)")
            off += 32
    return {"machine": MACHINES.get(machine, hex(machine)), "bits": bits,
            "subsystem": SUBSYSTEMS.get(subsystem, str(subsystem)), "imports": imports}


def classify(game_dir, verbose):
    modules = []
    redist = []
    info = None
    for root, dirs, files in os.walk(game_dir):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, game_dir)
            low = name.lower()
            if "redist" in rel.lower() or low.startswith(("vcredist", "dxsetup", "dotnet", "directx")) \
                    or low.endswith(".msi"):
                redist.append(rel)
            if low.startswith("goggame-") and low.endswith(".info") and info is None:
                try:
                    with open(path, encoding="utf-8-sig") as f:
                        info = json.load(f)
                except (OSError, ValueError):
                    pass
            if low.endswith((".exe", ".dll")):
                try:
                    pe = parse_pe(path)
                except (OSError, struct.error):
                    pe = None
                if pe:
                    pe["path"] = rel
                    pe["size"] = os.path.getsize(path)
                    modules.append(pe)
    print(f"== {game_dir}")
    if info:
        print(f"GOG info: name {info.get('name')!r}, osBitness {info.get('osBitness')}")
        for task in info.get("playTasks", []):
            print(f"  task {task.get('name')!r}: {task.get('path')} {task.get('arguments', '')} "
                  f"(primary={task.get('isPrimary', False)}, type={task.get('type')})")
    found = {}  # category -> set of descriptions
    by_machine = {}
    for m in modules:
        by_machine.setdefault(m["machine"], []).append(m["path"])
        tags = []
        for imp in m["imports"]:
            base = imp.split(" ")[0].lower()
            if base.endswith(".dll"):
                base = base[:-4]
            tag = KNOWN.get(base)
            if tag:
                cat, what = tag.split(":", 1)
                found.setdefault(cat, {}).setdefault(what, set()).add(m["path"])
                tags.append(what)
        kind = "exe" if m["path"].lower().endswith(".exe") else "dll"
        line = f"{kind} {m['machine']:6s} {m['path']}"
        if kind == "exe":
            line += f" [{m['subsystem']}]"
        if tags:
            line += "  <- " + ", ".join(sorted(set(tags)))
        print(line)
        if verbose:
            print("     imports: " + ", ".join(m["imports"]))
    # shipped module names are also APIs (e.g. a game that ships xaudio2_9redist.dll or OPENAL32.dll)
    for m in modules:
        base = os.path.basename(m["path"]).lower()[:-4]
        tag = KNOWN.get(base)
        if tag:
            cat, what = tag.split(":", 1)
            found.setdefault(cat, {}).setdefault(what + " (shipped)", set()).add(m["path"])
    print("-- summary")
    for machine, paths in sorted(by_machine.items()):
        exes = [p for p in paths if p.lower().endswith(".exe")]
        print(f"{machine}: {len(paths)} modules, {len(exes)} exe: {', '.join(os.path.basename(e) for e in exes)}")
    for cat in CATEGORY_ORDER:
        if cat in found:
            items = []
            for what, users in sorted(found[cat].items()):
                items.append(f"{what} ({len(users)})")
            print(f"{cat}: " + "; ".join(items))
    if redist:
        print("redistributables in the tree: " + ", ".join(redist))
    else:
        print("redistributables in the tree: none")


def main():
    args = sys.argv[1:]
    verbose = False
    if args and args[0] == "-v":
        verbose = True
        args = args[1:]
    if not args:
        print(__doc__)
        sys.exit(2)
    for d in args:
        classify(d, verbose)


if __name__ == "__main__":
    main()
