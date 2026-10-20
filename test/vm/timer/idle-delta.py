#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""idle-delta.py A.json B.json: per-vCPU and total growth of the KVM counters between two qstats.py
snapshots (interrupts injected, VM exits, halt wakeups)."""
import json
import sys

KEYS = ("irq_injections", "exits", "halt_wakeup", "halt_exits", "irq_exits")


def load(p):
    return {e["qom-path"]: {x["name"]: x["value"] for x in e["stats"]} for e in json.load(open(p))["return"]}


a, b = load(sys.argv[1]), load(sys.argv[2])
tot = dict.fromkeys(KEYS, 0)
for i, path in enumerate(sorted(a)):
    d = {k: b[path].get(k, 0) - a[path].get(k, 0) for k in KEYS}
    for k in KEYS:
        tot[k] += d[k]
    print("vcpu%d %s" % (i, " ".join("%s=%d" % kv for kv in d.items())))
print("total %s" % " ".join("%s=%d" % kv for kv in tot.items()))
