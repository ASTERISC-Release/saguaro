#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Check test/quick/core-tests.txt against libos/test/regression/test_libos.py without pytest or an install:
every id exists, there are no duplicates, and no id is conditional on SGX, ASAN, UBSAN, AVX, a test device
or one architecture (such a test would be skipped on some variant and the variants would not be comparable).
Also checks that vm_skip.toml and expected-failures.toml (libos entries) name existing tests.

Usage: test/quick/check-core-list.py [core-tests.txt [test_libos.py]]      exit status 0 if the list is fine
"""
import ast
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CONDITIONAL = ('HAS_SGX', 'ASAN', 'UBSAN', 'HAS_AVX', 'HAS_EDMM', 'IS_VM', 'ON_X86',
               "platform.machine()", "os.uname().machine", 'glob.glob')


def read_ids(path):
    with open(path) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith('#')]


def extract(path):
    """{id: decorators of the class and the method as source text} for every test_* method."""
    src = open(path).read()
    tree = ast.parse(src)
    tests = {}
    for cls in (n for n in tree.body if isinstance(n, ast.ClassDef)):
        cdec = [ast.get_source_segment(src, d) for d in cls.decorator_list]
        for fn in (n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name.startswith('test_')):
            tests[f'{cls.name}::{fn.name}'] = cdec + [ast.get_source_segment(src, d)
                                                       for d in fn.decorator_list]
    return tests


def main():
    core_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'core-tests.txt')
    test_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, 'libos/test/regression/test_libos.py')
    core = read_ids(core_path)
    tests = extract(test_path)
    bad = 0
    if len(core) != len(set(core)):
        print('duplicate ids in', core_path)
        bad = 1
    for c in core:
        decs = tests.get(c)
        if decs is None:
            print('unknown id', c)
            bad = 1
            continue
        text = ' '.join(decs)
        for kw in CONDITIONAL:
            if kw in text:
                print(f'conditional on {kw}: {c} -> {text[:100]}')
                bad = 1
    try:
        import tomllib as toml          # Python 3.11+
    except ImportError:
        import tomli as toml
    known = set()
    with open(os.path.join(REPO, 'libos/test/regression/vm_skip.toml'), 'rb') as f:
        for group in toml.load(f).get('group', []):
            known.update(group['tests'])
    with open(os.path.join(HERE, 'expected-failures.toml'), 'rb') as f:
        for entry in toml.load(f).get('known', []):
            if '::' in entry['test'] and entry.get('step') not in ('libos-fs', 'libos-abi'):
                known.add(entry['test'])
    for k in sorted(known - set(tests)):
        print('vm_skip.toml / expected-failures.toml name an unknown test:', k)
        bad = 1
    print(f'{len(core)} core ids, {len(tests)} tests in {os.path.basename(test_path)}:',
          'OK' if not bad else 'PROBLEMS')
    return bad


if __name__ == '__main__':
    sys.exit(main())
