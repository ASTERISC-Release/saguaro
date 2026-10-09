#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Fail if any ELF segment of the given files is both writable and executable (needs pyelftools).
# Usage: check-rwx.py FILE...

import sys

from elftools.elf.constants import P_FLAGS
from elftools.elf.elffile import ELFFile

if len(sys.argv) < 2:
    sys.exit(f'usage: {sys.argv[0]} FILE...')

failed = False
for path in sys.argv[1:]:
    with open(path, 'rb') as f:
        for i, segment in enumerate(ELFFile(f).iter_segments()):
            flags = segment.header.p_flags
            if flags & P_FLAGS.PF_X and flags & P_FLAGS.PF_W:
                print(f'error in {path}: segment {i} is both writable and executable')
                failed = True
sys.exit(1 if failed else 0)
