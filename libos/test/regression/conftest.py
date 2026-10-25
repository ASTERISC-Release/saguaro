# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""
Under the VM PAL (`SAGUARO_VM=1`, see dev docs: vm.md) tests listed in `vm_skip.toml` are skipped
(platform limitation) or marked xfail (known failure, cause not yet understood), each with a reason
category, so that the remaining result is a meaningful pass/fail signal for the VM PAL.

`SAGUARO_TESTSET=core` runs only the tests listed in `test/quick/core-tests.txt` (the LibOS part of the
regression tier of `test/quick.sh`, the same list on every variant); without it all tests are collected.
"""

import os
import pathlib
import platform
import warnings

import pytest
import tomli


# Lines that tests want shown at the end of the run (their own output is captured, and with
# --tb=line not printed for passing tests); filled by report_duration() in test_libos.py.
REPORTED_DURATIONS = []


def pytest_terminal_summary(terminalreporter):
    if REPORTED_DURATIONS:
        terminalreporter.section('reported durations')
        for line in REPORTED_DURATIONS:
            terminalreporter.write_line(line)


def _load_vm_entries():
    path = pathlib.Path(__file__).with_name('vm_skip.toml')
    with open(path, 'rb') as f:
        data = tomli.load(f)
    entries = {}
    machine = platform.machine()
    for group in data.get('group', []):
        # optional `arch = "x86_64"` (or a list): the group applies only on that architecture
        arch = group.get('arch')
        if arch is not None and machine not in ([arch] if isinstance(arch, str) else arch):
            continue
        for test in group['tests']:
            entries[test] = group
    return entries


def _core_ids():
    path = pathlib.Path(__file__).resolve().parents[3] / 'test' / 'quick' / 'core-tests.txt'
    with open(path) as f:
        return {l.strip() for l in f if l.strip() and not l.startswith('#')}


def _select_testset(config, items):
    testset = os.environ.get('SAGUARO_TESTSET', '')
    if testset == '':
        return
    if testset != 'core':
        raise pytest.UsageError(f'SAGUARO_TESTSET={testset}: the only known set is "core"')
    core = _core_ids()
    keep, drop = [], []
    for item in items:
        cls = getattr(item, 'cls', None)
        (keep if cls and f'{cls.__name__}::{item.name}' in core else drop).append(item)
    config.hook.pytest_deselected(items=drop)
    items[:] = keep


def pytest_collection_modifyitems(config, items):  # pylint: disable=unused-argument
    _select_testset(config, items)
    if os.environ.get('SAGUARO_VM') != '1':
        return
    # SAGUARO_VM_SKIPLIST=0 runs everything (raw pass/fail numbers, e.g. to compare architectures
    # or to see which listed tests have started passing)
    if os.environ.get('SAGUARO_VM_SKIPLIST', '1') == '0':
        return

    entries = _load_vm_entries()
    seen = set()
    for item in items:
        cls = getattr(item, 'cls', None)
        key = f'{cls.__name__}::{item.name}' if cls else item.name
        group = entries.get(key)
        if group is None:
            continue
        seen.add(key)
        reason = f'[VM:{group["category"]}] {group["reason"]}'
        if group['kind'] == 'skip':
            item.add_marker(pytest.mark.skip(reason=reason))
        else:
            item.add_marker(pytest.mark.xfail(reason=reason, strict=False))

    # a typo'd or renamed test silently losing its skip would be easy to miss
    for key in sorted(set(entries) - seen):
        warnings.warn(f'vm_skip.toml: no collected test matches "{key}"')
