#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Results of test/quick.sh: the CSV rows of a step, the one-line summary and the exit status.

  report.py add     --out DIR --variant V --arch A --commit C --tier T --vcpus N --step S --rc RC --secs SEC
                    [--junit FILE | --exec-log FILE | --summary-log FILE | --regress-results FILE]
  report.py summary --out DIR --variant V --arch A --commit C --tier T --vcpus N --secs SEC

results.csv has one row per test (a pytest step: from its JUnit file) or one row per step (any other step):
  variant,arch,commit,tier,step,test,result,seconds,vcpus,retried
  (retried is 1 for a step that quick.sh ran twice: only the helloworld example of sgx and vbs, on a known
  allocation failure of the enclave; the retry is printed and counted in the summary)
result is pass, fail, skip, xfail, xpass or known-fail. A failure is known-fail, and does not make the run fail,
only if expected-failures.toml has an entry that matches exactly: the variant is in `variants`, the
architecture is `arch` (when given), the vCPU count is in `vcpus` (when given), the step is `step` (when
given), and the test id (<class>::<test>, or the step name for a step that is not pytest) is `test`.
"""
import argparse
import csv
import os
import re
import sys
import xml.etree.ElementTree as ET

try:
    import tomllib as toml          # Python 3.11+
except ImportError:                 # pragma: no cover
    import tomli as toml

HERE = os.path.dirname(os.path.abspath(__file__))
FIELDS = ['variant', 'arch', 'commit', 'tier', 'step', 'test', 'result', 'seconds', 'vcpus']


def as_list(value):
    if value is None:
        return None
    return [value] if isinstance(value, (str, int)) else list(value)


def load_known():
    with open(os.path.join(HERE, 'expected-failures.toml'), 'rb') as f:
        return toml.load(f).get('known', [])


def is_known(known, args, test):
    for entry in known:
        if entry['test'] != test:
            continue
        if args.variant not in as_list(entry['variants']):
            continue
        if entry.get('arch') is not None and args.arch not in as_list(entry['arch']):
            continue
        if entry.get('vcpus') is not None and str(args.vcpus) not in [str(v) for v in as_list(entry['vcpus'])]:
            continue
        if entry.get('step') is not None and args.step not in as_list(entry['step']):
            continue
        return True
    return False


def xfail_ids():
    """The tests that vm_skip.toml marks xfail on this architecture: a pass of one of them is an XPASS."""
    import platform
    with open(os.path.join(HERE, '..', '..', 'libos', 'test', 'regression', 'vm_skip.toml'), 'rb') as f:
        groups = toml.load(f).get('group', [])
    ids = set()
    for group in groups:
        arch = as_list(group.get('arch'))
        if group['kind'] == 'xfail' and (arch is None or platform.machine() in arch):
            ids.update(group['tests'])
    return ids


def junit_rows(path):
    """(test id, result, seconds) of every testcase of a pytest JUnit file."""
    rows = []
    for case in ET.parse(path).getroot().iter('testcase'):
        cls = case.get('classname', '').split('.')[-1]
        test = f"{cls}::{case.get('name')}" if cls else case.get('name')
        result = 'pass'
        for child in case:
            if child.tag in ('failure', 'error'):
                result = 'fail'
            elif child.tag == 'skipped':
                result = 'xfail' if child.get('type') == 'pytest.xfail' else 'skip'
        rows.append((test, result, float(case.get('time', 0))))
    return rows


def exec_rows(path):
    """(example, result, seconds) from the [PASS], [FAIL] and [SKIP] lines that exec.sh prints."""
    rows = []
    with open(path, errors='replace') as f:
        for line in f:
            m = re.match(r'\[(PASS|FAIL|SKIP)\] (\S+)', line)
            if not m:
                continue
            secs = re.search(r'\((\d+)s\)', line)
            rows.append((m.group(2), {'PASS': 'pass', 'FAIL': 'fail', 'SKIP': 'skip'}[m.group(1)],
                         float(secs.group(1)) if secs else 0.0))
    return rows


def regress_rows(path):
    """(case, result, seconds) from the results.txt of pal/src/host/vbs/test/regress/run-regress.sh: one line per case,
    `<case>  pass|FAIL  exit=<code> <seconds>s`, then `passed N of M cases`."""
    rows = []
    with open(path, errors='replace') as f:
        for line in f:
            m = re.match(r'^(\S.*?)\s+(pass|FAIL)\s+exit=\S+\s+([\d.]+)s\s*$', line)
            if m:
                rows.append((m.group(1), 'pass' if m.group(2) == 'pass' else 'fail', float(m.group(3))))
    return rows


def summary_rows(path):
    """Rows from the plain pytest output (no JUnit file: the suites run in the RISC-V guest): the FAILED lines
    of -rf and the counts of the last summary line."""
    rows, text = [], open(path, errors='replace').read()
    for m in re.finditer(r'^FAILED \S*?(\w+::\w+)(?: |$)', text, re.M):
        rows.append((m.group(1), 'fail', 0.0))
    last = None
    for m in re.finditer(r'^=* ?((?:\d+ \w+(?:, )?)+)(?: in [\d.]+s.*)?=*$', text, re.M):
        if 'passed' in m.group(1) or 'failed' in m.group(1):
            last = m.group(1)
    counts = {k: int(n) for n, k in re.findall(r'(\d+) (passed|skipped|xfailed|deselected)', last or '')}
    for kind, result in (('passed', 'pass'), ('skipped', 'skip'), ('xfailed', 'xfail')):
        rows += [(f'({kind} {i + 1})', result, 0.0) for i in range(counts.get(kind, 0))]
    return rows


def cmd_add(args):
    known = load_known()
    rows = []
    if args.junit and os.path.exists(args.junit):
        try:
            rows = junit_rows(args.junit)
        except ET.ParseError:
            rows = []
    if not rows and args.summary_log and os.path.exists(args.summary_log):
        rows = summary_rows(args.summary_log)
    if not rows and args.regress_results and os.path.exists(args.regress_results):
        rows = regress_rows(args.regress_results)
    if not rows and args.exec_log and os.path.exists(args.exec_log):
        rows = exec_rows(args.exec_log)
    if not rows:
        rows = [(args.step, 'pass' if args.rc == 0 else 'fail', float(args.secs))]
    elif args.rc != 0 and not any(r[1] == 'fail' for r in rows):
        # the run failed without a failed test: a crash, a timeout or a collection error
        rows.append((args.step, 'fail', 0.0))
    counts = dict.fromkeys(['pass', 'fail', 'skip', 'xfail', 'xpass', 'known-fail'], 0)
    xf = xfail_ids() if args.variant in ('vm', 'sev', 'sev-snp') else set()
    with open(os.path.join(args.out, 'results.csv'), 'a', newline='') as f:
        writer = csv.writer(f, lineterminator='\n')
        for test, result, secs in rows:
            if result == 'fail' and is_known(known, args, test):
                result = 'known-fail'
            if result == 'pass' and test in xf:
                result = 'xpass'    # expected to fail (vm_skip.toml) but passed: flaky; reported, not a failure
            counts[result] += 1
            writer.writerow([args.variant, args.arch, args.commit, args.tier, args.step, test, result,
                             f'{secs:.2f}', args.vcpus, args.retried])
    print('{:<22} rc={:<3} {:>5}s  pass={pass} fail={fail} skip={skip} xfail={xfail} xpass={xpass} known={known-fail}'.format(
        args.step, args.rc, args.secs, **counts))


def expected_counts(args):
    """The measured libos-core row of expected.csv for this variant, architecture and vCPU count, or None."""
    with open(os.path.join(HERE, 'expected.csv'), newline='') as f:
        for row in csv.DictReader(f):
            if row['variant'] == args.variant and row['arch'] == args.arch \
                    and row['vcpus'] in (str(args.vcpus), '-') and row['measured'] == 'yes':
                return row
    return None


def cmd_summary(args):
    totals = dict.fromkeys(['pass', 'fail', 'skip', 'xfail', 'xpass', 'known-fail'], 0)
    core = dict.fromkeys(totals, 0)
    retried = set()
    with open(os.path.join(args.out, 'results.csv'), newline='') as f:
        for row in csv.DictReader(f):
            totals[row['result']] += 1
            if row['retried'] == '1':
                retried.add(row['step'])
            if row['step'] == 'libos-core':
                core[row['result']] += 1
    status = 'PASS' if totals['fail'] == 0 else 'FAIL'
    line = (f"QUICK variant={args.variant} arch={args.arch} tier={args.tier} vcpus={args.vcpus} "
            f"commit={args.commit} pass={totals['pass'] + totals['xpass']} fail={totals['fail']} skip={totals['skip']} "
            f"xfail={totals['xfail']} xpass={totals['xpass']} known={totals['known-fail']} retried={len(retried)} secs={args.secs} status={status}")
    with open(os.path.join(args.out, 'summary.txt'), 'w') as f:
        f.write(line + '\n')
    print(line)
    exp = expected_counts(args) if sum(core.values()) else None
    if exp and exp['pass'].isdigit():
        want = (int(exp['pass']), int(exp['skip']), int(exp['xfail']), int(exp['known_fail']))
        # a test that vm_skip.toml expects to fail and that passed (xpass) counts as an xfail here
        have = (core['pass'], core['skip'], core['xfail'] + core['xpass'], core['known-fail'])
        if want != have:
            print(f"NOTE libos-core pass/skip/xfail/known {have} differs from expected.csv {want}")
    sys.exit(0 if status == 'PASS' else 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['add', 'summary'])
    parser.add_argument('--out', required=True)
    parser.add_argument('--variant', required=True)
    parser.add_argument('--arch', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--tier', required=True)
    parser.add_argument('--vcpus', default='-')
    parser.add_argument('--step', default='')
    parser.add_argument('--rc', type=int, default=0)
    parser.add_argument('--secs', default='0')
    parser.add_argument('--retried', default='0')
    parser.add_argument('--junit')
    parser.add_argument('--exec-log')
    parser.add_argument('--summary-log')
    parser.add_argument('--regress-results')
    args = parser.parse_args()
    {'add': cmd_add, 'summary': cmd_summary}[args.command](args)


if __name__ == '__main__':
    main()
