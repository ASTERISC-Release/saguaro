# Guidelines for AI agents and contributors

This file is for any coding agent, whatever the tool, and for people. It is the single copy of
these guidelines; tool-specific files should only point here.

## Working in parallel

Several people or agents can work at once, each on their own branch. What makes that safe:

- **One checkout per branch.** Give each branch its own checkout (a clone or a git worktree) and do
  not work in another's. Switching branches inside a shared checkout while someone else uses it is
  what causes conflicts.
- **Name the folder `gramine-<name>`** (for example `gramine-graphics/gramine-multiarch`). Then
  `container/run.sh` names the container after the folder, gives it its own guest-CID base, and
  refuses a container that mounts a different checkout. Without such a folder it falls back to the
  default shared names, which suit one checkout only. `GRAMINE_WORKSPACE` sets the name by hand.
- **A QEMU fork** that a branch needs (see `Documentation/devel/graphics/`) sits next to the
  checkout as `qemu-tirith/`, or wherever `QEMU_TIRITH_DIR` points.
- **Push only your own branch.** Do not force-push or rewrite history that is already pushed. Leave
  the integration branch to whoever integrates.
- **Shared machine resources are not isolated by checkouts:** the screen, the hugepage pool, the
  CPUs (and so any timing), fixed test ports, CI minutes. Before you open a window, run a benchmark
  or start a test server, check that nobody else is using the same thing.

## How to work

- **Plan first.** Before a development task, write `Documentation/devel/<topic>/PLAN.md` (scope,
  phases with exit criteria, decisions with a recommendation, risks) and get it agreed.
- **Documents of a topic**, in `Documentation/devel/<topic>/`: `PLAN.md` (the current plan, edited
  in place), `FINDINGS.md` (the dated log, appended), `TODO.md` (future tasks) and `TESTING.md`
  (test results, with dates and conditions). More files are fine for how-tos. Keep
  `Documentation/devel/README.md` and `summary.md` current. Details in
  `Documentation/devel/README.md`.
- **Keep documents true.** After editing documents, run `scripts/check-docs.py`: it reports paths and links
  that name no file of the repository (names that are legitimately external go in `scripts/check-docs.allow`).
- **Commit small, verified steps** on your own branch, with a message that says what changed and
  why.
- **Ask first** before destructive git actions (`reset --hard`, deleting branches, anything on a
  remote), before spending CI minutes, before changing a branch that is not yours, and before
  removing a container or install prefix that holds work.
- **No root.** The development scripts use rootless containers; do not add steps that need `sudo`.
- **Shared code.** A change to a file used by several architectures or PALs is minimal, guarded by
  the architecture where possible, and checked with a build for the other targets before it is
  merged.

## Reporting

Say what you ran and what it showed. Do not report a result you did not measure; say what you did
not check. If a test fails, give the output. When something is done and verified, say so plainly.

## Where things are

- Building, containers and the RISC-V test guest: `INSTALL.rst`.
- Platform matrix, build options, how to test: `Documentation/devel/multiarch.md`.
- Development documents of every topic: `Documentation/devel/`.
