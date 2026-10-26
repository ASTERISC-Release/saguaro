# Contributing to Saguaro

Saguaro is experimental research software of the ASTERISC Research Lab (Arizona State University). Bug reports,
questions and pull requests are welcome.

## Before you send a change

- Keep a change to one topic, and match the style of the code around it (`.clang-format` and `.pylintrc` are in the
  root).
- Build and test what you touched: [INSTALL.md](INSTALL.md) and the guides in
  [Documentation/quickstart/](Documentation/quickstart/) say how, for example `test/quick.sh direct` (the tiers: [test/README.md](test/README.md)).
  Run `python3 scripts/check-docs.py` after changing documentation.
- Give new files an SPDX line and a copyright line, as the files next to them do. Code outside `CI-Examples/` is
  LGPL-3.0-or-later and the examples are BSD-3-Clause (see [LICENSE.addendum.txt](LICENSE.addendum.txt)). Do not
  copy in code whose license does not allow that.

## The QEMU submodules (`vmm/`)

The QEMU backends are forks of QEMU pinned as git submodules: `vmm/graphics`
([qemu-graphics](https://github.com/adilahmad17/qemu-graphics), branch `dev-graphics`), `vmm/sev-tcg`
([qemu-sev-tcg](https://github.com/adilahmad17/qemu-sev-tcg), branch `dev-sev`) and `vmm/tdx-tcg`
([qemu-tdx-tcg](https://github.com/adilahmad17/qemu-tdx-tcg), branch `dev-tdx`). A clone does not fetch them;
`scripts/qemu.sh fetch <name>` checks out the pinned commit (shallow), `scripts/qemu.sh build <name>` builds it
into `vmm/<name>/prefix`, and `scripts/qemu.sh status` lists the three.

To bump a pin, commit and push the change in the fork first, then:

    git -C vmm/<name> fetch origin <branch>
    git -C vmm/<name> checkout <sha>
    git add vmm/<name>

and commit. A pin must stay reachable from the fork's branch: never force-push a fork branch.
`scripts/check-submodules.sh` checks `.gitmodules` and the pins.

## AI-generated code and content

Any use of an AI tool (code assistant, chat model, code or image generator) that produced code, tests, comments,
documentation or other content of your contribution must be marked by you, the contributor:

- add a trailer to each commit that contains such content, naming the tool and model, for example
  `Assisted-by: <tool>:<model>`, and
- state in the pull request description what was generated and how you checked it.

You are responsible for every line you submit: you must understand it, have tested it, and be sure that it may be
published under the license of the file. Contributions with unmarked AI-generated content may be declined.
