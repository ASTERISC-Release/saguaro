Quickstart guides
=================

One short guide per platform, each from a fresh checkout to a running example, with the commands
that were run on a machine of that platform (each guide ends with what was verified, where and
when). The complete instructions are in `INSTALL.rst <../../INSTALL.rst>`__; the platform table
and status of this tree are in `README.rst <../../README.rst>`__.

Symbols as in ``README.rst``: ✅ supported, ⚠️ experimental, 🛫 planned, ❌ not supported, ➖ not
applicable.

.. list-table::
   :header-rows: 1
   :widths: 34 14 14 14 14 10

   * - What
     - Intel x86_64
     - AMD64
     - AArch64
     - RISC-V (RV64)
     - Guide
   * - ``gramine-direct``: an ordinary Linux process; hello world, regression tests
     - ✅
     - ✅
     - ✅
     - ⚠️ `riscv.rst <riscv.rst>`__
     - `direct.rst <direct.rst>`__
   * - ``gramine-vm``: a microVM under KVM; processes, VM size, networking
     - ✅
     - ⚠️
     - ⚠️
     - 🛫
     - `vm.rst <vm.rst>`__
   * - ``gramine-direct`` with the host GPU: glxgears, Quake II, Vulkan
     - ⚠️
     - ⚠️
     - ⚠️ (NVIDIA)
     - ❌
     - `graphics-direct.rst <graphics-direct.rst>`__
   * - ``gramine-vm`` with graphics redirection: glxgears, Quake II (OpenGL, Vulkan)
     - ⚠️
     - ⚠️
     - 🛫
     - ❌
     - `graphics-vm.rst <graphics-vm.rst>`__
   * - Windows VBS enclaves (``gramine-vbs``): hello world, Python, attestation
     - ❌
     - ❌
     - ⚠️ (Windows 11)
     - ➖
     - `vbs-windows.rst <vbs-windows.rst>`__
   * - Intel TDX and SGX (inherited; not verified here)
     - ✅
     - ➖
     - ➖
     - ➖
     - `tdx-sgx.rst <tdx-sgx.rst>`__

Which one do I need?
--------------------

- **Just trying Gramine, or developing it**: `direct.rst <direct.rst>`__. It needs only
  ``podman`` (or the build packages) and works anywhere, also in a VM or a container.
- **The application should not run on the host's kernel**: `vm.rst <vm.rst>`__. It needs a
  physical Linux machine of the same architecture with KVM.
- **A game or another OpenGL/Vulkan program**: on your own desktop and with trusted code,
  `graphics-direct.rst <graphics-direct.rst>`__ (fastest, but not isolated; also AArch64 with
  NVIDIA); inside a microVM on x86_64, `graphics-vm.rst <graphics-vm.rst>`__ (needs the QEMU
  fork).
- **A RISC-V build**, without RISC-V hardware: `riscv.rst <riscv.rst>`__.
- **An application protected from Windows itself**, on an ARM64 Windows 11 PC:
  `vbs-windows.rst <vbs-windows.rst>`__.
- **Intel confidential computing**: `tdx-sgx.rst <tdx-sgx.rst>`__, pointers to the inherited
  documents.
