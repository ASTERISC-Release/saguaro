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

## AI-generated code and content

Any use of an AI tool (code assistant, chat model, code or image generator) that produced code, tests, comments,
documentation or other content of your contribution must be marked by you, the contributor:

- add a trailer to each commit that contains such content, naming the tool and model, for example
  `Assisted-by: <tool>:<model>`, and
- state in the pull request description what was generated and how you checked it.

You are responsible for every line you submit: you must understand it, have tested it, and be sure that it may be
published under the license of the file. Contributions with unmarked AI-generated content may be declined.
