#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""compare-sev.py OUTDIR [--min-delta S] [--csv FILE]: summarize the runs of measure-sev.sh.

OUTDIR holds before/rK/ and after/rK/ (K = 1, 2, ...; the warm-up run "rw" is ignored). Prints the
LibOS suite totals and wall time per vCPU count and run, the per-test comparison of the median
duration over the runs (tests that got slower by more than --min-delta seconds, by name), the
probe medians and maxima, and the idle interrupt counts."""
import argparse
import collections
import glob
import os
import re
import statistics
import sys
import xml.etree.ElementTree as ET


def runs(out, tree):
    return sorted(d for d in glob.glob(os.path.join(out, tree, "r[0-9]*")))


def suite(path):
    """-> ({test: seconds}, {test: outcome}, total seconds, counts)"""
    root = ET.parse(path).getroot()
    suite = root if root.tag == "testsuite" else root.find("testsuite")
    times, outcome = {}, {}
    for tc in suite.iter("testcase"):
        name = tc.get("classname", "").split(".")[-1] + "::" + tc.get("name")
        times[name] = float(tc.get("time"))
        kids = {c.tag: c for c in tc}
        if "failure" in kids or "error" in kids:
            outcome[name] = "failed"
        elif "skipped" in kids:
            t = kids["skipped"].get("type", "")
            outcome[name] = "xfailed" if "xfail" in t or "xfail" in (kids["skipped"].get("message") or "") else "skipped"
        else:
            outcome[name] = "passed"
    return times, outcome, float(suite.get("time", 0)), suite


def log_totals(path):
    try:
        text = open(path, errors="replace").read()
    except OSError:
        return "?"
    m = re.findall(r"^=*\s*(\d+ (?:failed|passed|skipped|xfailed|xpassed|error)[^=]*?) in ([\d.]+)s", text, re.M)
    return "%s in %ss" % m[-1] if m else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--min-delta", type=float, default=0.5)
    ap.add_argument("--csv")
    a = ap.parse_args()
    for n in (1, 2, 4):
        print("\n## LibOS suite, %d vCPU" % n)
        per = {}
        for tree in ("before", "after"):
            per[tree] = collections.defaultdict(list)
            for d in runs(a.out, tree):
                x = os.path.join(d, "libos-%d.xml" % n)
                if not os.path.exists(x):
                    continue
                times, outcome, total, _ = suite(x)
                print("%-6s %s: %s" % (tree, os.path.basename(d), log_totals(os.path.join(d, "libos-%d.log" % n))))
                for t, v in times.items():
                    per[tree][t].append((v, outcome[t]))
        names = sorted(set(per["before"]) & set(per["after"]))
        rows = []
        for t in names:
            b = statistics.median(v for v, _ in per["before"][t])
            f = statistics.median(v for v, _ in per["after"][t])
            ob = collections.Counter(o for _, o in per["before"][t]).most_common(1)[0][0]
            of = collections.Counter(o for _, o in per["after"][t]).most_common(1)[0][0]
            rows.append((t, b, f, f - b, ob, of))
        if not rows:
            continue
        sb, sf = sum(r[1] for r in rows), sum(r[2] for r in rows)
        print("sum of per-test medians: before %.1f s, after %.1f s (%+.1f s)" % (sb, sf, sf - sb))
        slower = sorted((r for r in rows if r[3] > a.min_delta), key=lambda r: -r[3])
        faster = sorted((r for r in rows if r[3] < -a.min_delta), key=lambda r: r[3])
        print("slower by more than %.1f s: %d, faster: %d" % (a.min_delta, len(slower), len(faster)))
        for r in slower:
            print("  SLOWER %-70s %7.2f -> %7.2f  (%+.2f)  %s/%s" % (r[0], r[1], r[2], r[3], r[4], r[5]))
        for r in faster[:15]:
            print("  faster %-70s %7.2f -> %7.2f  (%+.2f)  %s/%s" % (r[0], r[1], r[2], r[3], r[4], r[5]))
        chg = [r for r in rows if r[4] != r[5]]
        for r in chg:
            print("  OUTCOME CHANGED %s: %s -> %s" % (r[0], r[4], r[5]))
        if a.csv:
            with open("%s-%d.csv" % (a.csv, n), "w") as f:
                f.write("test,before_s,after_s,delta_s,before_outcome,after_outcome\n")
                for r in rows:
                    f.write("%s,%.3f,%.3f,%.3f,%s,%s\n" % r)
    print("\n## probes (tickgran/threadlat), median over runs of each reported number")
    for n in (1, 2, 4):
        for tree in ("before", "after"):
            vals = collections.defaultdict(list)
            for d in runs(a.out, tree):
                p = os.path.join(d, "probes-%d.log" % n)
                if os.path.exists(p):
                    for line in open(p):
                        m = re.match(r"PROBE (\w+): INFO (.*?)\s+median\s+([\d.]+) ms, max\s+([\d.]+) ms", line)
                        if m:
                            vals[(m[1], m[2])].append((float(m[3]), float(m[4])))
                        m = re.match(r"PROBE threadlat: INFO (.*): median ([\d.]+) ms, p90 ([\d.]+) ms, max ([\d.]+) ms", line)
                        if m:
                            vals[("threadlat", m[1])].append((float(m[2]), float(m[4])))
            for k, v in sorted(vals.items()):
                print("%d vCPU %-6s %-9s %-55s median %8.2f  max-of-max %8.2f  (%d runs; medians %s)" % (
                    n, tree, k[0], k[1], statistics.median(x for x, _ in v), max(y for _, y in v), len(v),
                    ",".join("%.2f" % x for x, _ in v)))
    print("\n## idle interrupts injected by KVM in 30 s (total over vCPUs; vcpu0 in parentheses)")
    for f in sorted({os.path.basename(p) for t in ("before", "after") for d in runs(a.out, t)
                     for p in glob.glob(os.path.join(d, "idle-*-p*.txt"))}):
        for tree in ("before", "after"):
            v = []
            for d in runs(a.out, tree):
                p = os.path.join(d, f)
                if os.path.exists(p):
                    t = open(p).read()
                    tot = int(re.search(r"total irq_injections=(\d+)", t)[1])
                    c0 = int(re.search(r"vcpu0 irq_injections=(\d+)", t)[1])
                    ex = int(re.search(r"total irq_injections=\d+ exits=(\d+)", t)[1])
                    v.append((tot, c0, ex))
            print("%-18s %-6s injections %s  exits %s" % (f, tree, ", ".join("%d(%d)" % (x, y) for x, y, _ in v),
                  ", ".join(str(z) for _, _, z in v)))


main()
