*****************************************************
Gramine Library OS with Multi-Architectural Support
*****************************************************

*A Linux-compatible library OS for multi-process applications, on Intel x86_64, AMD64 and AArch64,
with an experimental RISC-V (RV64) port.*

Gramine is a lightweight library OS: it runs an unmodified Linux application
in an isolated environment, with its own implementation of the Linux system
call interface (the *LibOS*) on top of a small host abstraction layer (the
*PAL*). This tree extends `gramine-tdx
<https://github.com/gramineproject/gramine-tdx>`__ -- Gramine with Intel TDX and
virtual-machine backends -- so that it also runs on AArch64 and, experimentally,
on RISC-V, and keeps upstream's full history.

Gramine (formerly *Graphene*) was designed and built by a diverse group of
researchers and engineers from universities and industry. Please see acknowledgements section below.

Status
======

This is a Gramine fork for development, research and teaching. It is **experimental, comes with no
warranty and should not be used in production**.

Current work:

- multi-architecture support: AMD64, AArch64 and RISC-V
- graphics through redirection to the host GPU (see `Tirith <https://arxiv.org/abs/2609.17525>`__)
- richer system call support (for example `uFork <https://arxiv.org/pdf/2509.09439>`__)

What it is for:

- **Sandboxing applications.** An unmodified Linux application runs on Gramine's own system call
  interface, either as an ordinary process (``gramine-direct``) or, with the ``vm`` PAL, in a minimal
  virtual machine (a *microVM*) that holds only the application, the library OS and a small kernel.
- **Running microVM workloads**, such as games with the (experimental) graphics redirection, or
  services that should not see the host operating system.
- **Running one application stack on x86_64, AArch64 and RISC-V**, to compare behaviour and
  performance.
- **Studying and teaching operating-system design.** The library OS and the small bare-metal kernel
  (page tables, scheduler, interrupts, virtio drivers, SMP) are readable, with design logs under
  ``Documentation/devel/``.
- **Using the inherited confidential-computing back ends** (Intel TDX and SGX) on x86_64.

The features added so far, and the planned ones, are listed in
`Documentation/FEATURES.md <Documentation/FEATURES.md>`__. Not available yet: a RISC-V ``vm`` PAL,
graphics on AArch64, delivery of asynchronous signals to blocked threads in the ``vm`` PAL, and a
finer-grained timer. The AArch64 ``vm`` PAL runs single-process applications; TCP and UDP work
through a vsock relay. See `Documentation/TODO.md <Documentation/TODO.md>`__ for open items.

The RISC-V port covers the ``direct`` PAL only and is **experimental**: it is cross-built, and its
regression suites and example applications pass in an emulated RISC-V Linux guest
(``qemu-system-riscv64``), but it has not been run on RISC-V hardware.

Platform support
================


==========  ===================================================================
Symbol      Meaning
==========  ===================================================================
✅          supported: works and is covered by tests
⚠️          experimental: runs, but incomplete, with known limits, or not yet fully validated
🛫          planned: on the roadmap, not started or not usable yet
❌          not supported: possible in principle, but it does not work (yet)
➖          not applicable: the combination does not exist
==========  ===================================================================

.. list-table::
   :header-rows: 1
   :widths: 36 16 16 16 16

   * - PAL (backend)
     - Intel x86_64
     - AMD64
     - AArch64
     - RISC-V (RV64)
   * - ``direct`` (``gramine-direct``): an ordinary Linux process
     - ✅
     - ✅
     - ✅
     - ⚠️
   * - ``vm`` (``gramine-vm``): a unikernel in a QEMU guest
     - ✅
     - ⚠️
     - ⚠️
     - 🛫
   * - ↳ ``vm`` with ``graphics`` (host redirection)
     - ⚠️
     - ⚠️
     - 🛫
     - ❌
   * - ``tdx`` (Intel TDX) and ``sgx`` (Intel SGX)
     - ✅
     - ➖
     - ➖
     - ➖

Per-cell details, build options and the verification status are in
`Documentation/devel/multiarch.md <Documentation/devel/multiarch.md>`__.

Installation
============

See `INSTALL.rst <INSTALL.rst>`__ for how to build and install Gramine on a host or in
the supplied containers, for the direct and the ``vm`` PAL, on x86_64 and AArch64. It also
describes the cross-build and the test guest of the RISC-V port.

Once installed, run an application::

    cd CI-Examples/helloworld
    make
    gramine-direct helloworld

``CI-Examples/`` contains ready-made applications;
``CI-Examples/install-all-prereqs.sh`` installs the host packages that some of them
(curl, gcc, iperf, mongodb, nodejs, openjdk, pytorch, r) need.

Documentation
=============

- `Documentation index <Documentation/README.md>`__: what each document is, and which are inherited
  from upstream.
- `Features <Documentation/FEATURES.md>`__: what this tree adds to gramine-tdx, and what is planned.
- `Development documents <Documentation/devel/>`__: one directory per port or feature, with its plan,
  log, open items and test results; `summary <Documentation/devel/summary.md>`__ gives the outcome of
  each on one page.
- `Multi-architecture notes <Documentation/devel/multiarch.md>`__: configurations, build options,
  tests, benchmarks and CI workflows.
- `Differences from upstream gramine-tdx <Documentation/devel/graphics/tirith-delta.md>`__.
- `Graphics examples <CI-Examples/graphics/>`__ (x86_64) and how to run them:
  `Documentation/devel/graphics/ <Documentation/devel/graphics/>`__.
- For features shared with Gramine in general (manifest syntax, ...) the
  `upstream documentation <https://gramine.readthedocs.io>`__ is a useful reference; details may
  differ in this tree.

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

