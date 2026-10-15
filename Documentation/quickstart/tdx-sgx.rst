Quickstart: Intel TDX and SGX (inherited, not verified here)
=============================================================

This tree keeps the confidential-computing back ends of upstream `gramine-tdx
<https://github.com/gramineproject/gramine-tdx>`__ for Intel CPUs: ``gramine-sgx`` (the application
in an SGX enclave) and ``gramine-tdx`` (the ``vm`` unikernel in a TDX trust domain). **Neither has
been run on SGX or TDX hardware as part of this tree's work**; use the upstream documentation and
treat this page as pointers.

Status (as in `README.rst <../../README.rst>`__): Intel x86_64 ✅ (inherited); AMD64, AArch64 and
RISC-V ➖.

SGX
---

Needs an Intel CPU with SGX (FLC), the in-kernel SGX driver (``/dev/sgx_enclave``) and, for remote
attestation, Intel's DCAP or EPID services. The SGX build also needs the protobuf-c tools, which
``scripts/dependencies.sh`` does not install (``protobuf-c-compiler libprotobuf-c-dev``)::

    scripts/install.sh --sgx --prefix ~/.local/gramine-sgx --build-dir build-sgx
    . ~/.local/gramine-sgx/env.sh
    is-sgx-available                   # what the CPU and kernel offer
    gramine-sgx-gen-private-key        # once: the enclave signing key
    cd CI-Examples/helloworld
    make SGX=1                         # renders, then signs: helloworld.manifest.sgx, helloworld.sig
    gramine-sgx helloworld

Reference: `sgx-intro.rst <../sgx-intro.rst>`__, `sgx-setup.rst <../sgx-setup.rst>`__,
`attestation.rst <../attestation.rst>`__, the ``sgx.*`` keys of `manifest-syntax.rst
<../manifest-syntax.rst>`__, and `gramine.readthedocs.io <https://gramine.readthedocs.io>`__.

TDX
---

Needs an Intel CPU and host kernel with TDX, a QEMU with TDX support and the TD-shim firmware, as
described by the upstream gramine-tdx project. The build is ``scripts/install.sh --vm --tdx``; the
launcher is ``gramine-tdx`` (the TDX variant of ``gramine-vm``, ``tools/gramine-vm.in``).

Known issue (2026-10-15): the ``tdx`` PAL does not build at ``9e28ea81``
(``pal/src/host/vm-common/pal_common_sockets.c``: ``invalid application of 'sizeof' to incomplete
type 'struct udp_rx_ring'``): the network code shared by the vm PALs uses a structure that only
the ``vm`` PAL's host header defines.

Verified
--------

2026-10-15, ``main`` at ``9e28ea81``, in the x86_64 container on a machine **without** SGX or TDX:
after installing ``protobuf-c-compiler libprotobuf-c-dev protobuf-compiler python3-protobuf`` in
the container, ``scripts/install.sh --sgx ...`` built and installed; ``is-sgx-available`` printed
``SGX supported by CPU: false``; ``gramine-sgx-gen-private-key`` and ``make SGX=1`` produced the
signed ``helloworld.manifest.sgx`` and ``helloworld.sig``; ``gramine-sgx helloworld`` stopped with
``Cannot open SGX driver device``, as expected without SGX. ``scripts/install.sh --tdx`` and
``--vm --tdx`` failed with the build error above.

Not verified: running anything under SGX or TDX, attestation, and the TDX QEMU and firmware.
