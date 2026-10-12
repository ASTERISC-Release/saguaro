# Rules for AI agents (and humans) working in this repository

This file is for every coding agent, whatever the tool. It is the single copy of the rules; do not
duplicate them in tool-specific files. If you are an agent, read it before you change anything.

## Workspaces: several agents at once

Work happens in separate checkouts, one per branch, under `~/Projects/gramine/`:

| Folder | Branch | Used for |
|---|---|---|
| `gramine-main/` | `main` | integration: merging, CI, releases, fixes that belong on every branch |
| `gramine-dev-riscv/` | `dev-riscv` | all general RISC-V development |
| `gramine-graphics/` | `dev-graphics` | the graphics host redirection |

Each folder contains `gramine-multiarch/` and, where graphics VMs run, a sibling `qemu-tirith/` (the
QEMU fork). A new topic that needs its own branch gets a new folder named `gramine-<topic>`.

- Work only in the checkout you were started in. Do not read or change a sibling folder, and do not
  use its containers.
- One branch per folder. Push only that branch. `main` is pushed only from `gramine-main`. Never
  force-push, and never rewrite history that is already pushed.
- Containers are named after the folder (`container/run.sh` does this): `gramine-main-vm-x86_64`,
  `gramine-dev-riscv-riscv64-cross`, and so on. `run.sh` refuses a container that mounts a
  different checkout. Do not work around that with `CONTAINER_NAME`.
- Things the folders cannot isolate, because they belong to the machine: the screen, the hugepage
  pool, the CPUs (and with them any timing), fixed test ports, GitHub Actions minutes. Before you
  open a window, run a benchmark or start a test server, check that no other agent is doing the same
  (for example with `flock /tmp/gramine-gui.lock <command>`). Tell the user before a game or
  other window opens on their display.

## How to work

- **Plan first.** Before a development task, write `Documentation/devel/<topic>/PLAN.md` (scope,
  phases with exit criteria, decisions with a recommendation, risks) and get it agreed.
- **Documents of a topic**, in `Documentation/devel/<topic>/`, always these four:
  `PLAN.md` (the current plan, edited in place), `FINDINGS.md` (the dated log, appended),
  `TODO.md` (future tasks), `TESTING.md` (test results, with dates and conditions). Extra
  files are fine for how-tos. Update `Documentation/devel/README.md` and `summary.md` when a topic
  is added or changes status. See `Documentation/devel/README.md`.
- **Commit and push after each major completed step**, to your own branch, without being asked.
  Small, verified steps; the message says what changed and why.
- **Ask first** before: destructive git actions (`reset --hard`, deleting branches, anything on a
  remote), spending GitHub Actions minutes, changing another branch, removing a container or an
  install prefix that holds work.
- **Run everything unprivileged** where possible: rootless containers, no `sudo`.
- **Keep shared code safe.** Changes to files used by several architectures or PALs are minimal,
  guarded by the architecture where possible, and checked against a build for the other targets
  (x86_64 direct and vm, and a riscv64 cross-build) before they are merged.
- **Files derived from Tirith** mention the ASTERISC Research Lab in their header.

## Reporting

Say what you ran and what it showed. Do not report a result you did not measure; say what you did
not check. If a test fails, give the output. When something is done and verified, say so plainly.

## Where things are

- Building, containers and the RISC-V test guest: `INSTALL.rst`.
- Platform matrix, build options, how to test: `Documentation/devel/multiarch.md`.
- Development documents of every topic: `Documentation/devel/`.
