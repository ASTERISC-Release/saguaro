*****************************************************
Gramine Library OS with Multi-Architectural Support
*****************************************************

*A Linux-compatible library OS (LibOS) for multi-process applications, on Intel x86_64, AMD64,
AArch64 and RISC-V.*

Gramine (formerly *Graphene*) runs an unmodified Linux application in an isolated environment, with its own
implementation of the Linux system call interface (the *LibOS*) on top of a small host abstraction
layer (the *PAL*). It was initially designed and built by a diverse group of researchers
and engineers from universities and industry to run on Intel x86 platforms. Please see the
`Acknowledgements`_ section below and the `upstream repository
<https://github.com/gramineproject/gramine>`__.

This fork has been significantly extended to support additional architectures, backends and
features. It extends `gramine-tdx <https://github.com/gramineproject/gramine-tdx>`__ -- Gramine
with Intel TDX and virtual-machine backends -- and keeps upstream's full history. It is implemented and maintained by the `ASU Trusted, Reliable, and Secure Computing
(ASTERISC) Research Lab <https://asteriscresearchlab.com/>`__. This fork supports several host environments:

* Containerized userspace (``direct``)
* Isolated userspace (``sgx``)
* Micro-virtual machine (``vm``)
* Windows protected virtual machine (``vbs``)
* Confidential virtual machine (``tdx``, ``sev``)

Use-Cases
=========


**(1) Sandboxing untrusted Linux applications (e.g., AI agents and games):** An application runs
on Gramine's own system call interface rather than directly on the host kernel, reducing the
host interface that the application can reach. The ``direct`` PAL runs it as a lightweight 
container-like environment. The ``vm`` PAL runs it in a minimal virtual machine that holds only 
the application, the library OS and a small kernel. Graphics-intensive applications are supported through
redirection to the host GPU in an efficient manner.

**(2) Running unmodified Linux applications in diverse protected contexts:** The same library OS
runs an application inside hardware-protected environments, so that it is protected from the
platform that hosts it: Intel SGX enclaves (``sgx``), Intel TDX and AMD SEV confidential virtual
machines (``tdx``, ``sev``) and Windows VBS enclaves (``vbs``). The application does not need to be modified
or ported to a different enclave interface. Remote attestation lets a remote party verify what is running.

**(3) Teaching Linux-inspired operating system implementations on different architectures:** The
library OS and the small bare-metal kernel of the ``vm`` PAL (page tables, scheduler, interrupts,
and virtio drivers) are compact and readable. The same Linux-compatible interface is implemented
on x86_64, AArch64 and RISC-V, so that the architecture-specific parts can be compared directly.


Status
======

This is a research and teaching fork. It is **experimental, comes with no warranty and should not
be used in production**. Current work is focused on the following:

- **Multi-architecture:** AMD64, AArch64 and RISC-V.
- **Graphics:** redirection to the host GPU in the ``vm``/``direct`` PAL (see `Tirith
  <https://arxiv.org/abs/2609.17525>`__).
- **Windows VBS enclaves:** running applications inside VBS enclaves on Windows 11 (ARM64 and x64).
- **Windows applications:** Wine and Proton in the ``vm`` PAL (a 64-bit game reaches its first level in
  software rendering; the GPU path and 32-bit games are in progress).
- **System call support:** richer coverage, including multi-process, signaling, and network.

The features added so far and the planned/future ones are listed in
`Documentation/FEATURES.md <Documentation/FEATURES.md>`__ and
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

Sandboxing Environments
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 36 16 16 16 16

   * - Platform Adaptation Layer (PAL)
     - Intel x86_64
     - AMD64
     - AArch64
     - RISC-V (RV64)
   * - ``direct``
     - ✅
     - ✅
     - ✅
     - ⚠️
   * - ``direct`` with redirected graphics
     - ⚠️
     - ⚠️
     - ⚠️
     - ❌
   * - ``vm`` (``gramine-vm``)
     - ✅
     - ⚠️
     - ⚠️
     - 🛫
   * - ``vm`` with redirected graphics
     - ⚠️
     - ⚠️
     - 🛫
     - ❌

Protected Computing
-------------------

.. list-table::
   :header-rows: 1
   :widths: 36 16 16 16 16

   * - Platform Adaptation Layer (PAL)
     - Intel x86_64
     - AMD64
     - AArch64
     - RISC-V (RV64)
   * - ``sgx`` (Intel SGX)
     - ⚠️
     - ➖
     - ➖
     - ➖
   * - ``tdx`` (Intel TDX)
     - ⚠️
     - ➖
     - ➖
     - ➖
   * - ``sev`` (AMD SEV)
     - ➖
     - ⚠️
     - ➖
     - ➖
   * - ``vbs`` (Windows VBS enclaves)
     - ⚠️
     - ⚠️
     - ⚠️
     - ➖

``sgx`` and ``tdx`` are inherited from Gramine and gramine-tdx: they are built (``tdx`` in CI), but this
tree has not run them on hardware, so they are marked experimental.
``sev`` (``gramine-sev``, the vm PAL as an AMD SEV-SNP or plain SEV guest) runs the LibOS regression
suite and attestation on an EPYC (Milan) host; it is experimental: no CI runner has SEV, and the
host-supplied files are not yet checked (Documentation/devel/sev/).

Please refer to per-cell details, build options and verification status: `Documentation/devel/multiarch.md <Documentation/devel/multiarch.md>`__.

Getting Started
===============

Each quickstart guide goes from a fresh checkout to a running example, with the commands verified
on a machine of that platform:

- **Containerized userspace** (``direct``): `Documentation/quickstart/direct.rst
  <Documentation/quickstart/direct.rst>`__
- **Micro-virtual machine** (``vm``): `Documentation/quickstart/vm.rst
  <Documentation/quickstart/vm.rst>`__
- **Windows VBS enclaves** (``vbs``): `Documentation/quickstart/vbs-windows.rst
  <Documentation/quickstart/vbs-windows.rst>`__
- **Intel TDX and SGX** (``tdx``, ``sgx``): `Documentation/quickstart/tdx-sgx.rst
  <Documentation/quickstart/tdx-sgx.rst>`__
- **AMD SEV-SNP** (``sev``): `Documentation/quickstart/sev.rst
  <Documentation/quickstart/sev.rst>`__

Guides for graphics and for the RISC-V cross-build are listed in the `quickstart index
<Documentation/quickstart/README.rst>`__. 

`INSTALL.rst <INSTALL.rst>`__ is the complete reference
for building and installing, on a host or in the supplied containers.

``CI-Examples/`` contains ready-made applications. To check whether your own application is likely
to run, see `Will my application run? <Documentation/will-it-run.rst>`__.

Documentation
=============

Using this tree:

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

Reference:

- `Features <Documentation/FEATURES.md>`__: what this tree adds to gramine-tdx, and what is planned.
- `Multi-architecture notes <Documentation/devel/multiarch.md>`__: configurations, build options,
  tests, benchmarks and CI workflows.
- `Syscall compatibility matrix <Documentation/devel/syscall-compat/MATRIX.md>`__.
- `Differences from upstream gramine-tdx <Documentation/devel/graphics/tirith-delta.md>`__.
- `Upstream documentation <https://gramine.readthedocs.io>`__ for features shared with Gramine
  (manifest syntax and others); details may differ in this tree.

Development:

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

Citation
========

This work is the culmination of several research projects undertaken by our lab, including:

- ``Tirith`` 
- ``Erebor``
- ``Whitepaper``

(TODO: complete later)

Acknowledgements
================

This work builds on `Gramine <https://github.com/gramineproject/gramine>`__ and its
Intel TDX and virtual-machine fork `gramine-tdx
<https://github.com/gramineproject/gramine-tdx>`__. We thank the Gramine and gramine-tdx authors and contributors; the organizations and individuals are listed in
`ACKNOWLEDGEMENTS.rst <ACKNOWLEDGEMENTS.rst>`__.

