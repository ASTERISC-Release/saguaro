#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Check the development documents against the repository.

Reports every file path, markdown link and reStructuredText link in the checked documents that does not name
a file of the repository (by path, path suffix or file name), and, when docutils is installed, every
reStructuredText warning in README.rst, INSTALL.rst and Documentation/quickstart/. Names that are
legitimately not in the repository (files of the external QEMU fork, kernel paths, historical names) are
listed in scripts/check-docs.allow.

Usage:  scripts/check-docs.py [--no-rst] [FILE...]      (default: the documents of Documentation/devel/README.md)
Exit status: 0 when nothing is reported, 1 otherwise.
"""

import argparse
import glob
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# Dated logs (kept as written) and the historical plan are not checked: they name things as they were.
DEFAULT_GLOBS = [
    'README.rst', 'INSTALL.rst', 'AGENTS.md',
    'Documentation/quickstart/*.rst',
    'Documentation/README.md', 'Documentation/TODO.md', 'Documentation/FEATURES.md',
    'Documentation/devel/README.md', 'Documentation/devel/summary.md', 'Documentation/devel/syscall-workflow.md',
    'Documentation/devel/multiarch.md', 'Documentation/devel/vm.md',
    'Documentation/devel/*/PLAN.md', 'Documentation/devel/*/TODO.md', 'Documentation/devel/*/TESTING.md',
    'Documentation/devel/aarch64/*/PLAN.md', 'Documentation/devel/aarch64/*/TODO.md',
    'Documentation/devel/aarch64/*/TESTING.md',
    'Documentation/devel/graphics/running.md', 'Documentation/devel/graphics/tirith-delta.md',
    'Documentation/devel/syscall-compat/MATRIX.md',
    'Documentation/devel/syscall-compat/README.md',
    'Documentation/devel/syscall-compat/RERUN.md',
]
DOC_EXT = ('.md', '.rst', '.sh', '.py', '.toml', '.yml')
# tokens that are not paths at all: placeholders, options, urls, absolute system paths, code
NOT_A_PATH = re.compile(r'[<>*$%{}|;=\s]|^\.\.?$|^-|^~|^/(dev|proc|sys|tmp|usr|etc|lib|root|var|home|bin|opt)\b'
                        r'|^[a-z]+://|^\.\.\.|^\w+\.\w+\(')
PATH_CHARS = re.compile(r'^[A-Za-z0-9_.+/\-]+$')


def tracked_files():
    out = subprocess.run(['git', 'ls-files'], capture_output=True, text=True, check=True).stdout
    return out.split('\n')


def load_allow():
    patterns = []
    with open(os.path.join(ROOT, 'scripts', 'check-docs.allow')) as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line:
                patterns.append(re.compile(line))
    return patterns


def exists(token, base, files, basenames):
    token = token.rstrip('.,:;)')
    stripped = token.lstrip('./')
    for prefix in ('', 'Documentation/', 'Documentation/devel/', os.path.dirname(base) + '/'):
        path = os.path.normpath(os.path.join(ROOT, prefix, token))
        if os.path.exists(path) or glob.glob(path):
            return True
    if '/' not in stripped:
        return stripped in basenames
    return any(f == stripped or f.endswith('/' + stripped) or f.startswith(stripped) for f in files)


def tokens_of(text):
    toks = set(re.findall(r'`([^`\n]+)`', text))
    toks |= set(re.findall(r'\]\(([^)#\s]+)(?:#[^)]*)?\)', text))
    toks |= set(re.findall(r'<([^<>\s`]+)>`?__', text))
    return toks


def check_paths(docs, allow):
    files = tracked_files()
    basenames = {os.path.basename(f) for f in files}
    problems = 0
    for doc in docs:
        text = open(doc, errors='replace').read()
        missing = []
        for tok in sorted(tokens_of(text)):
            tok = tok.strip()
            if NOT_A_PATH.search(tok) or not PATH_CHARS.match(tok):
                continue
            if '/' not in tok and not tok.endswith(DOC_EXT):
                continue
            if any(p.search(tok) for p in allow):
                continue
            if not exists(tok, doc, files, basenames):
                missing.append(tok)
        if missing:
            problems += len(missing)
            print(f'{doc}: not in the repository:')
            for tok in missing:
                print(f'    {tok}')
    return problems


def check_rst(docs):
    try:
        import docutils.core
    except ImportError:
        print('docutils is not installed: reStructuredText not checked')
        return 0
    problems = 0
    for doc in docs:
        err = io.StringIO()
        docutils.core.publish_string(open(doc).read(), source_path=doc, writer_name='null',
                                     settings_overrides={'report_level': 2, 'halt_level': 5,
                                                         'warning_stream': err})
        msgs = [m for m in err.getvalue().splitlines() if m.strip()]
        if msgs:
            problems += len(msgs)
            print(f'{doc}: reStructuredText warnings:')
            for m in msgs[:8]:
                print(f'    {m[:150]}')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='*')
    ap.add_argument('--no-rst', action='store_true', help='skip the reStructuredText check')
    args = ap.parse_args()

    docs = args.files
    if not docs:
        for pattern in DEFAULT_GLOBS:
            docs += sorted(glob.glob(pattern))
    docs = [d for d in docs if os.path.isfile(d)]

    problems = check_paths(docs, load_allow())
    if not args.no_rst:
        problems += check_rst([d for d in docs if d in ('README.rst', 'INSTALL.rst')
                               or d.startswith('Documentation/quickstart/')])
    print(f'{len(docs)} documents checked, {problems} problem(s)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
