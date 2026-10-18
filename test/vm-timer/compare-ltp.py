#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""compare-ltp.py BEFORE.json AFTER.json [--min-delta S]: per-test duration_seconds of two
ltp-sharded.py results files (the serial row of a rerun test; the parallel row of the others, and the
parallel row of every test separately), the status changes, and the totals."""
import argparse
import collections
import json
import statistics


def load(p):
    d = json.load(open(p))
    return d, {r["test"]: r for r in d["results"]}


ap = argparse.ArgumentParser()
ap.add_argument("before")
ap.add_argument("after")
ap.add_argument("--min-delta", type=float, default=1.0)
a = ap.parse_args()
db, b = load(a.before)
da, f = load(a.after)
for name, d in (("before", db), ("after", da)):
    print("%s: commit %s, wall %.1f s, test_seconds %.1f, load %s, parallel_totals %s" % (
        name, d["commit"], d["wall_seconds"], d["test_seconds"], d.get("load"), d.get("parallel_totals")))
    print("   final status: %s; serial rerun wall %.1f s" % (
        dict(collections.Counter(r["status"] for r in d["results"])), d["serial_rerun"]["wall_seconds"]))
names = sorted(set(b) & set(f))


def par(r):  # the parallel-run duration
    return r.get("parallel", r)["duration_seconds"]


for label, get in (("final row (serial where rerun)", lambda r: r["duration_seconds"]),
                   ("parallel row (8 shards, tests that passed in both)", par)):
    # a parallel row of a test that failed in the parallel run (and was rerun) is a quick failure
    # of the shared host, not a duration: only tests that passed in both parallel runs are compared
    only = (lambda t: b[t].get("parallel", b[t])["status"] == "pass" == f[t].get("parallel", f[t])["status"]) \
        if get is par else (lambda t: True)
    rows = [(t, get(b[t]), get(f[t])) for t in names if only(t)]
    sb, sf = sum(r[1] for r in rows), sum(r[2] for r in rows)
    print("\n## %s: sum before %.1f s, after %.1f s (%+.1f s), median per-test delta %+.3f s" % (
        label, sb, sf, sf - sb, statistics.median(r[2] - r[1] for r in rows)))
    slower = sorted((r for r in rows if r[2] - r[1] > a.min_delta), key=lambda r: r[1] - r[2])
    print("slower by more than %.1f s: %d of %d; faster: %d" % (
        a.min_delta, len(slower), len(rows), sum(1 for r in rows if r[1] - r[2] > a.min_delta)))
    for t, x, y in slower:
        print("  SLOWER %-34s %8.2f -> %8.2f (%+.2f)  %s/%s" % (t, x, y, y - x, b[t]["status"], f[t]["status"]))
chg = [(t, b[t]["status"], f[t]["status"]) for t in names if b[t]["status"] != f[t]["status"]]
print("\n## final status changes: %d" % len(chg))
for t, x, y in chg:
    print("  %-34s %s -> %s" % (t, x, y))
