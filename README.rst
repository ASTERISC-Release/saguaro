*****************************************************
Gramine Library OS with Multi-Architectural Support
*****************************************************

*A Linux-compatible library OS for multi-process applications, on Intel x86_64, AMD64 and AArch64.*

Status
======

This project is, at this point, a system designed for **research and teaching
purposes**. It is **experimental** and **must not be used in production
environments**. It comes with no warranty.

What is it?
===========

Gramine is a lightweight library OS: it runs an unmodified Linux application
in an isolated environment, with its own implementation of the Linux system
call interface (the *LibOS*) on top of a small host abstraction layer (the
*PAL*). This tree extends `gramine-tdx
<https://github.com/gramineproject/gramine-tdx>`__ -- Gramine with Intel TDX and
virtual-machine backends -- so that it also runs on AArch64, and keeps
upstream's full history.

Supported configurations
========================

.. list-table::
   :header-rows: 1
   :widths: 22 26 26 26

   * - PAL (backend)
     - Intel x86_64
     - AMD64
     - AArch64
   * - ``direct`` (``gramine-direct``, an ordinary Linux process)
     - ⚠️ inherited from upstream; being re-validated here
     - ✅ inherited from upstream; being re-validated here
     - ✅ works; regression suites and example applications pass
   * - ``vm`` (``gramine-vm``, a unikernel in a QEMU guest)
     - ✅ inherited from upstream
     - ⚠️ partly validated (work in progress)
     - ❌ planned
   * - ``tdx`` (Intel TDX), ``sgx`` (Intel SGX)
     - ✅ inherited from upstream
     - ❌ not applicable
     - ❌ not applicable

Details, build options and the verification status are in
`Documentation/devel/multiarch.md <Documentation/devel/multiarch.md>`__.

Quick start
===========

On Debian/Ubuntu, from the repository root::

    scripts/dependencies.sh           # host packages (uses sudo)
    scripts/install.sh                # build, install into ~/.local/gramine, write its env.sh
    . ~/.local/gramine/env.sh         # put the install on PATH / PYTHONPATH (this shell)
    scripts/test.sh                   # regression tests and example applications

Run an application::

    cd CI-Examples/helloworld
    make
    gramine-direct helloworld

``scripts/install.sh --help`` lists the options (install prefix, ``--vm``, ``--graphics
disabled``, ...). ``CI-Examples/`` contains ready-made applications;
``CI-Examples/install-all-prereqs.sh`` installs the host packages that some of them
(curl, gcc, iperf, mongodb, nodejs, openjdk, pytorch, r) need.

Documentation
=============

- `Multi-architecture notes
  <Documentation/devel/multiarch.md>`__: supported configurations, build options,
  test and benchmark tooling.
- `Differences from upstream gramine-tdx
  <Documentation/devel/tirith-delta.md>`__.
- `AArch64 port notes <Documentation/devel/aarch64/>`__: design, findings, test
  parity with x86_64.
- For features shared with Gramine in general (manifest syntax, ...), the
  `upstream documentation <https://gramine.readthedocs.io>`__ is a useful
  reference; details may differ in this tree.

License
=======

Gramine is licensed under the LGPL-3.0-or-later (see ``LICENSE.txt``); the
examples in ``CI-Examples/`` are BSD-3-Clause unless a file says otherwise (see
``LICENSE.addendum.txt``). Files added in this tree carry ``Copyright (C) 2026
The ASTERISC Research Lab``; the copyright notices of upstream Gramine and
gramine-tdx are retained.
