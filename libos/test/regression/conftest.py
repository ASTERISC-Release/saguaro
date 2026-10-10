# SPDX-License-Identifier: LGPL-3.0-or-later
"""
Under the VM PAL (`GRAMINE_VM=1`, see README-vm.md) tests listed in `vm_skip.toml` are skipped
(platform limitation) or marked xfail (known failure, cause not yet understood), each with a reason
category, so that the remaining result is a meaningful pass/fail signal for the VM PAL.
"""

import os
import pathlib
import warnings

import pytest
import tomli


def _load_vm_entries():
    path = pathlib.Path(__file__).with_name('vm_skip.toml')
    with open(path, 'rb') as f:
        data = tomli.load(f)
    entries = {}
    for group in data.get('group', []):
        for test in group['tests']:
            entries[test] = group
    return entries


def pytest_collection_modifyitems(config, items):  # pylint: disable=unused-argument
    if os.environ.get('GRAMINE_VM') != '1':
        return
    # GRAMINE_VM_SKIPLIST=0 runs everything (raw pass/fail numbers, e.g. to compare architectures
    # or to see which listed tests have started passing)
    if os.environ.get('GRAMINE_VM_SKIPLIST', '1') == '0':
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
