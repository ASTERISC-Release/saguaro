# Quickstart: Containerized Userspace (`gramine-direct`)

`gramine-direct` runs an unmodified Linux application as a container-like Linux process on top
of the Gramine LibOS.

All guides use one route: the development container of `container/run.sh`. Building and installing
on the host is not described.

## Status

Symbols as in [README.md](../../README.md):

| Configuration | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|----|----|----|----|----|
| `gramine-direct` | ✅ | ✅ | ✅ | ⚠️ |
| `gramine-direct` with redirected graphics | ⚠️ | ⚠️ | ⚠️ | ❌ |

## Requirements

- `podman`, rootless. No other package is installed on the host, and no root access is needed.
- A Linux host of the architecture to be tested (Intel x86_64, AMD64 or AArch64). The direct PAL
  needs no device: it also works inside a virtual machine or another container. RISC-V is covered
  by [riscv.md](riscv.md).
- Checkout location: a folder `gramine-<name>/gramine-multiarch` (see `Documentation/AGENTS.md`) keeps the
  container names of several checkouts apart.

# 1. Container and install

`container/run.sh` builds an Ubuntu 24.04 image for the architecture of this machine with every
build dependency (the first call takes a few minutes), creates a long-lived container and attaches
a shell. The repository is mounted at `/root/gramine-multiarch`. Three forms exist:

    cd <repo-home>
    container/run.sh                    # opens a shell in the container
    container/run.sh '<command>'        # runs one command in the container and returns
    container/run.sh --target riscv64   # the RISC-V cross container (see riscv.md)

The commands in the next sections run in the container shell, in the repository, or are given as
`container/run.sh '<command>'` from the host. `scripts/install.sh` without an option builds the
direct backend and installs it into `~/.local/gramine`:

    container/run.sh 'scripts/install.sh'

The `~` is the home directory of the container's root user; it stays when the container is
stopped and is discarded when the container is recreated (the builds inside the repository
survive). Re-running the script is cheap: meson and ninja rebuild only what changed.
`container/run.sh` loads only the environment of the vm install (`~/.local/gramine-vm/env.sh`), so
`gramine-direct` of this install is not on `PATH` by itself: `. ~/.local/gramine/env.sh` sets `PATH` and
`PYTHONPATH` in the current shell, and `scripts/install.sh --add-to-profile` does it for every new shell.

# 2. Example: Hello World

Every directory of `CI-Examples/` has an `exec.sh`. It builds the example, runs it under the
chosen backend and checks the output. In the container shell:

    cd CI-Examples/helloworld
    ./exec.sh --direct

A backend is required (`--direct`, `--vm`, `--sev`, `--sev-snp`, `--sgx`, `--vbs`); without one the script
lists the backends and whether this host can run each. The script prints each command it runs
(lines that start with `+`) and writes the log to `/tmp/gramine-exec/`. The exit status is 0 on a
pass. The run ends with the output `Hello, world`.

The same steps by hand, which show what `exec.sh` does (the program is built, `helloworld.manifest`
is rendered from its template, and `gramine-direct` runs the program on the LibOS):

    . ~/.local/gramine/env.sh             # gramine-direct and the tools on PATH
    cd CI-Examples/helloworld
    make                                  # builds the program and renders helloworld.manifest
    gramine-direct helloworld

The output is `Hello, world`. The other examples run the same way (`./exec.sh --direct` in their
directory); the recorded results are in
[RESULTS.md](../../CI-Examples/RESULTS.md): every example passes under `direct` on Intel x86_64 and on
AArch64; `blender` runs on x86_64 only.

# 3. Regression Tests

The test scripts find the install through `SAGUARO_PREFIX` (default `~/.local/gramine`):

    test/direct/test-direct.sh --list                       # the suites
    test/direct/test-direct.sh pal libos helloworld python  # a quick selection, under a minute
    test/direct/test-direct.sh                              # everything: PAL, LibOS, 12 example suites

Each suite ends with a `PASS` or `FAIL` line. Logs are stored in `/tmp/gramine-multiarch-tests`.
`scripts/install.sh --run-tests pal libos` builds and runs the tests in one step.

# 4. Troubleshooting

- `gramine-direct: command not found`: run `. ~/.local/gramine/env.sh` in that shell
  (`container/run.sh` loads only the vm install's environment), or `scripts/install.sh --add-to-profile` once.
- `exec.sh` reports that a backend is not installed: run `scripts/install.sh` (direct) or
  `scripts/install.sh --vm` (vm) in the container, as in section 1.
- An application cannot find a file or library: the manifest mounts only what it names
  (`fs.mounts`); add the missing path, as the examples do. `make DEBUG=1` or
  `loader.log_level = "debug"` shows what the library OS looked for.
- `container/run.sh` refuses a container that mounts a different checkout: name the folder
  `gramine-<name>/gramine-multiarch` or set `SAGUARO_WORKSPACE` (see `Documentation/AGENTS.md`).
