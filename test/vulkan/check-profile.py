#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Check a device's `vulkaninfo --json` export against a Vulkan profile file.

Usage: check-profile.py PROFILE.json VULKANINFO.json [PROFILE_NAME ...]

PROFILE.json is a Vulkan profiles file such as DXVK's VP_DXVK_requirements.json or VKD3D-Proton's
VP_D3D12_VKD3D_PROTON_profile.json. VULKANINFO.json is the output of `vulkaninfo --json=N -o FILE`.
Without profile names, every profile in the file is checked. For each profile it prints PASS or the
missing extensions, features, properties and format features.

The comparison is flattened: a feature or property member counts as present if any structure of
the device reports it (core VkPhysicalDeviceVulkan1xFeatures and the extension structures carry the
same members). Numeric properties: members named min* or *Alignment/*Granularity must be <= the
profile's value, all others >=. Flag lists must be supersets. Queue families are not checked. This is an approximation of the
Vulkan Profiles layer's check, good enough to list what a driver lacks.
"""
import json
import sys


def flatten(structs):
    flat = {}
    for members in structs.values():
        for name, value in members.items():
            if name in flat and isinstance(value, bool):
                flat[name] = flat[name] or value
            else:
                flat.setdefault(name, value)
    return flat


def smaller_is_better(name):
    return (name.startswith("min") or name.endswith("Alignment") or name.endswith("Granularity")
            or name in ("nonCoherentAtomSize", "bufferImageGranularity"))


def cmp_value(name, req, have):
    if have is None:
        return False
    if isinstance(req, bool):
        return (not req) or bool(have)
    if isinstance(req, list):
        if req and isinstance(req[0], str):
            return set(req) <= set(have if isinstance(have, list) else [have])
        if not isinstance(have, list) or len(have) != len(req):
            return False
        return all(cmp_value(name, r, h) for r, h in zip(req, have))
    if isinstance(req, (int, float)):
        return have <= req if smaller_is_better(name) else have >= req
    if isinstance(req, str):
        return req == have
    if isinstance(req, dict):
        return all(cmp_value(k, v, (have or {}).get(k)) for k, v in req.items())
    return req == have


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    prof = json.load(open(sys.argv[1]))
    info = json.load(open(sys.argv[2]))
    dev = info["capabilities"]["device"]
    dev_api = list(info["profiles"].values())[0]["api-version"]
    dev_ext = set(dev.get("extensions", {}))
    dev_feat = flatten(dev.get("features", {}))
    dev_prop = flatten(dev.get("properties", {}))
    dev_fmt = dev.get("formats", {})
    label = list(info["profiles"].values())[0]["label"]
    names = sys.argv[3:] or list(prof["profiles"])
    print(f"device: {label}, API {dev_api}")
    worst = 0
    for pname in names:
        p = prof["profiles"][pname]
        missing = []
        req_api = p.get("api-version", "0")
        if tuple(map(int, req_api.split("."))) > tuple(map(int, dev_api.split("."))):
            missing.append(f"api-version {req_api}")
        for cap in p["capabilities"]:
            caps = [cap] if isinstance(cap, str) else cap  # a list means "any of"
            results = []
            for c in caps:
                block = prof["capabilities"][c]
                m = []
                for ext in block.get("extensions", {}):
                    if ext not in dev_ext:
                        m.append(f"extension {ext}")
                for struct in block.get("features", {}).values():
                    for name, val in struct.items():
                        if not cmp_value(name, val, dev_feat.get(name)):
                            m.append(f"feature {name}")
                for struct in block.get("properties", {}).values():
                    for name, val in struct.items():
                        have = dev_prop.get(name)
                        if isinstance(val, dict) and isinstance(have, dict):
                            for k, v in val.items():
                                if not cmp_value(k, v, have.get(k)):
                                    m.append(f"property {name}.{k}: need {v}, have {have.get(k)}")
                        elif not cmp_value(name, val, have):
                            m.append(f"property {name}: need {val}, have {have}")
                for fmt, req in block.get("formats", {}).items():
                    have = dev_fmt.get(fmt)
                    if have is None:
                        m.append(f"format {fmt}: not reported")
                        continue
                    hp = flatten(have)
                    for struct in req.values():
                        for name, val in struct.items():
                            if not cmp_value(name, val, hp.get(name)):
                                m.append(f"format {fmt} {name}")
                results.append((c, m))
            ok = [r for r in results if not r[1]]
            if not ok:
                for c, m in results:
                    missing += [f"[{c}] {x}" for x in m]
        if missing:
            worst = 1
            print(f"  {pname}: MISSING {len(missing)}")
            for x in missing:
                print(f"    {x}")
        else:
            print(f"  {pname}: PASS")
    return worst


if __name__ == "__main__":
    sys.exit(main())
