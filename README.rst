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

Gramine (formerly *Graphene*) was designed and built by a diverse group of
researchers and engineers from universities and industry (the retained copyright
notices include Stony Brook University, Texas A&M University, Intel and Invisible
Things Lab).

Platform support
================

Legend:

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
     - 🛫
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
the supplied containers, for the direct and the ``vm`` PAL, on x86_64 and AArch64.

Once installed, run an application::

    cd CI-Examples/helloworld
    make
    gramine-direct helloworld

``CI-Examples/`` contains ready-made applications;
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
