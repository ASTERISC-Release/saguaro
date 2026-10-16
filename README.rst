*****************************************************
Gramine Library OS with Multi-Architectural Support
*****************************************************

*A Linux-compatible library OS (LibOS) for multi-process applications, on Intel x86_64, AMD64,
AArch64 and RISC-V.*

Gramine runs an unmodified Linux application in an isolated environment, with its own
implementation of the Linux system call interface (the *LibOS*) on top of a small host abstraction
layer (the *PAL*). This fork supports several PALs:

* Containerized userspace (``direct``)
* Isolated userspace (``sgx``)
* Micro-virtual machine (``vm``)
* Windows protected virtual machine (``vbs``)
* Confidential virtual machine (``tdx``, ``sev``)

This tree extends `gramine-tdx <https://github.com/gramineproject/gramine-tdx>`__ -- Gramine with
Intel TDX and virtual-machine backends -- and keeps upstream's full history.

Gramine (formerly *Graphene*) was initially designed and built by a diverse group of researchers
and engineers from universities and industry to run on Intel x86 platforms. Please see the
`Acknowledgements`_ section below and the `upstream repository
<https://github.com/gramineproject/gramine>`__.

This fork has been significantly extended to support additional architectures, backends and
features. It is implemented and maintained by the ASTERISC Research Lab.

.. TODO: link the ASTERISC Research Lab web page here.

Rationale
=========

**Sandboxing untrusted Linux applications** (for example AI agents and games). An application runs
on Gramine's own system call interface rather than directly on the host kernel, which reduces the
host interface that the application can reach. The ``direct`` PAL runs it as an ordinary host
process, which is lightweight but is not a security boundary. The ``vm`` PAL runs it in a minimal
virtual machine that holds only the application, the library OS and a small kernel, and the host
operating system is not visible to it. Graphics-intensive applications are supported through
redirection to the host GPU, as described in `Tirith <https://arxiv.org/abs/2609.17525>`__.

**Running unmodified Linux applications in protected computing contexts.** The same library OS
runs an application inside hardware-protected environments, so that it is protected from the
platform that hosts it: Intel SGX enclaves (``sgx``), Intel TDX and AMD SEV confidential virtual
machines (``tdx``, ``sev``) and Windows VBS enclaves (``vbs``). The application is not modified
or ported to a different enclave interface. Attestation lets a remote party verify what is running.

**Teaching Linux-inspired operating system implementations on different architectures.** The
library OS and the small bare-metal kernel of the ``vm`` PAL (page tables, scheduler, interrupts,
virtio drivers, SMP) are compact and readable. The same Linux-compatible interface is implemented
on x86_64, AArch64 and RISC-V, so that the architecture-specific parts can be compared directly.
Design logs for every component are in ``Documentation/devel/``.

Status and Current Focus
========================

This is a research and teaching fork. It is **experimental, comes with no warranty and should not
be used in production**. Current work:

- **Multi-architecture support:** AMD64, AArch64 and RISC-V.
- **Graphics:** redirection to the host GPU in the ``vm`` PAL (see `Tirith
  <https://arxiv.org/abs/2609.17525>`__), and, experimentally, direct use of the host GPU by
  ``gramine-direct`` on x86_64 and AArch64.
- **Windows VBS enclaves:** running applications inside VBS enclaves on Windows 11 (experimental,
  ARM64 and x64).
- **System call support:** richer coverage, for example `uFork <https://arxiv.org/pdf/2509.09439>`__.

The features added so far and the planned ones are listed in
`Documentation/FEATURES.md <Documentation/FEATURES.md>`__; open items are in
`Documentation/TODO.md <Documentation/TODO.md>`__.

Platform Support
================

.. list-table::
   :widths: 14 86

   * - ✅
     - supported: works and is covered by tests
   * - ⚠️
     - experimental: runs, but incomplete, with known limits, or not yet fully validated
   * - 🛫
     - planned: on the roadmap, not started or not usable yet
   * - ❌
     - not supported: possible in principle, but it does not work (yet)
   * - ➖
     - not applicable: the combination does not exist

Sandboxing
----------

.. list-table::
   :header-rows: 1
   :widths: 36 16 16 16 16

   * - PAL
     - Intel x86_64
     - AMD64
     - AArch64
     - RISC-V (RV64)
   * - ``direct`` (``gramine-direct``)
     - ✅
     - ✅
     - ✅
     - ⚠️
   * - ``direct`` with graphics (host GPU)
     - ⚠️
     - ⚠️
     - ⚠️
     - ❌
   * - ``vm`` (``gramine-vm``)
     - ✅
     - ⚠️
     - ⚠️
     - 🛫
   * - ``vm`` with graphics (host redirection)
     - ⚠️
     - ⚠️
     - 🛫
     - ❌

Protected Computing
-------------------

.. list-table::
   :header-rows: 1
   :widths: 36 16 16 16 16

   * - PAL
     - Intel x86_64
     - AMD64
     - AArch64
     - RISC-V (RV64)
   * - ``sgx`` (Intel SGX)
     - ✅
     - ➖
     - ➖
     - ➖
   * - ``tdx`` (Intel TDX)
     - ✅
     - ➖
     - ➖
     - ➖
   * - ``sev`` (AMD SEV)
     - ➖
     - 🛫
     - ➖
     - ➖
   * - ``vbs`` (Windows VBS enclaves, ``gramine-vbs``)
     - ⚠️
     - ❌
     - ⚠️
     - ➖

Notes:

- ``direct`` is an ordinary Linux process and is **not a sandbox**; the graphics keys give the
  application access to the host GPU. On RISC-V it is cross-built and passes its test suites in
  an emulated guest, but has not been run on hardware.
- ``sgx`` and ``tdx`` are inherited from gramine-tdx and have not been run on SGX or TDX hardware
  here; the ``tdx`` PAL is compiled in CI only. ``sev`` is planned and has no code yet.
- ``vbs`` needs Windows 11 with VBS enabled and the Windows-side tools described in
  `Documentation/vbs.rst <Documentation/vbs.rst>`__.
- Per-cell details, build options and verification status:
  `Documentation/devel/multiarch.md <Documentation/devel/multiarch.md>`__.

Getting Started
===============

Each quickstart guide goes from a fresh checkout to a running example, with the commands verified
on a machine of that platform:

- **Sandboxing, as a host process** (``direct``): `Documentation/quickstart/direct.rst
  <Documentation/quickstart/direct.rst>`__
- **Sandboxing, in a microVM** (``vm``): `Documentation/quickstart/vm.rst
  <Documentation/quickstart/vm.rst>`__
- **Windows VBS enclaves** (``vbs``): `Documentation/quickstart/vbs-windows.rst
  <Documentation/quickstart/vbs-windows.rst>`__
- **Intel TDX and SGX** (``tdx``, ``sgx``): `Documentation/quickstart/tdx-sgx.rst
  <Documentation/quickstart/tdx-sgx.rst>`__

Guides for graphics and for the RISC-V cross-build are listed in the `quickstart index
<Documentation/quickstart/README.rst>`__. `INSTALL.rst <INSTALL.rst>`__ is the complete reference
for building and installing, on a host or in the supplied containers.

Once installed, run an application::

    cd CI-Examples/helloworld
    make
    gramine-direct helloworld

``CI-Examples/`` contains ready-made applications. To check whether your own application is likely
to run, see `Will my application run? <Documentation/will-it-run.rst>`__.

Documentation
=============

*Using this tree*

- `Quickstart guides <Documentation/quickstart/README.rst>`__: one per platform.
- `INSTALL.rst <INSTALL.rst>`__: building, installing, containers, the RISC-V cross-build.
- `Will my application run? <Documentation/will-it-run.rst>`__: the syscall profiler and the
  tolerance check.
- `Windows VBS enclaves <Documentation/vbs.rst>`__: the manual (installation on Windows, the
  ``gramine-vbs`` tool, attestation, limits).
- Graphics: in ``gramine-direct`` on `x86_64 <Documentation/devel/graphics-direct/>`__ and
  `AArch64 <Documentation/devel/graphics-aarch64/>`__, in the ``vm`` PAL on
  `x86_64 <Documentation/devel/graphics/>`__; examples in `CI-Examples/graphics/
  <CI-Examples/graphics/>`__.

*Reference*

- `Features <Documentation/FEATURES.md>`__: what this tree adds to gramine-tdx, and what is planned.
- `Multi-architecture notes <Documentation/devel/multiarch.md>`__: configurations, build options,
  tests, benchmarks and CI workflows.
- `Syscall compatibility matrix <Documentation/devel/syscall-compat/MATRIX.md>`__.
- `Differences from upstream gramine-tdx <Documentation/devel/graphics/tirith-delta.md>`__.
- `Upstream documentation <https://gramine.readthedocs.io>`__ for features shared with Gramine
  (manifest syntax and others); details may differ in this tree.

*Development*

- `Documentation index <Documentation/README.md>`__: what each document is, and which are inherited
  from upstream.
- `Development documents <Documentation/devel/>`__: one directory per port or feature, with its
  plan, log, open items and test results; the `summary <Documentation/devel/summary.md>`__ gives
  the outcome of each on one page.

Contributing
============

Bug reports and pull requests are welcome through the GitHub repository. By
submitting a contribution you agree that it is licensed under the license of the
file it modifies or adds (LGPL-3.0-or-later, or BSD-3-Clause for files under
``CI-Examples/``), and that you have the right to submit it under that license.

License
=======

Gramine is licensed under the LGPL-3.0-or-later (see ``LICENSE.txt``); the
examples in ``CI-Examples/`` are BSD-3-Clause unless a file says otherwise (see
``LICENSE.addendum.txt``). The copyright notices of upstream Gramine and gramine-tdx
are retained.

Acknowledgements
================

This work builds on `Gramine <https://github.com/gramineproject/gramine>`__ and its
Intel TDX and virtual-machine fork `gramine-tdx
<https://github.com/gramineproject/gramine-tdx>`__. We thank the Gramine and
gramine-tdx authors and contributors; the organizations and individuals are listed in
`ACKNOWLEDGEMENTS.rst <ACKNOWLEDGEMENTS.rst>`__.

