Quickstart: ``gramine-vm`` on x86_64 and AArch64
=================================================

``gramine-vm`` boots the library OS, a small kernel and the application as a unikernel in a QEMU
guest (a *microVM*): the application no longer runs on the host's kernel. Details:
`INSTALL.rst <../../INSTALL.rst>`__ and `Documentation/devel/vm.md <../devel/vm.md>`__.

Status (as in `README.rst <../../README.rst>`__): Intel x86_64 ✅, AMD64 ⚠️, AArch64 ⚠️
(single-process); RISC-V 🛫 (not available).

Requirements
------------

- A **native** Linux machine of the same architecture with hardware virtualisation: ``/dev/kvm``
  readable and writable by your user. Emulation (TCG) is not supported, and a VM without nested
  virtualisation will not do.
- For guest networking: ``/dev/vhost-vsock`` (module ``vhost_vsock``), also accessible to your user.
- ``podman`` for the container route below, or the packages of the host route.

1. Build in the container (recommended)
---------------------------------------

The container image already has QEMU, ``virtiofsd`` and the toolchain::

    container/run.sh                   # shell in the container; the rest runs inside
    scripts/install.sh --vm            # build and install into ~/.local/gramine-vm
    . ~/.local/gramine-vm/env.sh

On x86_64 ``--vm`` builds the direct PAL too, and the graphics paths (`graphics-vm.rst
<graphics-vm.rst>`__); ``--graphics disabled`` leaves them out. On AArch64 it builds only the vm
PAL.

**Host route** (Debian/Ubuntu, needs sudo for the packages; commands from ``INSTALL.rst``)::

    sudo apt install qemu-system-x86 virtiofsd lsof python3-psutil       # x86_64
    sudo apt install qemu-system-arm virtiofsd device-tree-compiler      # AArch64
    scripts/dependencies.sh --build-only
    scripts/install.sh --vm
    . ~/.local/gramine-vm/env.sh

On x86_64, if ``command -v virtiofsd`` finds nothing, link ``/usr/libexec/virtiofsd`` into
``/usr/local/bin``.

2. Hello world
--------------

::

    cd CI-Examples/helloworld
    make                               # against the vm install: the manifest names its libsysdb.so
    gramine-vm helloworld

The output ends with::

    Hello, world
    [ VM exited with code 0 ]

How it works: ``make`` renders ``helloworld.manifest`` from the template; with the vm install's
``env.sh`` loaded, the manifest's loader entry point is the vm build of the library OS.
``gramine-vm helloworld`` reads ``helloworld.manifest``, starts ``virtiofsd`` (the guest sees the
container's or host's ``/`` as its ``/``) and a vsock relay, and boots QEMU with the Gramine kernel.
The application's exit status comes back in the ``[ VM exited with code N ]`` line (134 means a
guest kernel panic) and is ``gramine-vm``'s own exit status. On AArch64 the application binary must
be a PIE; the examples' Makefiles build it that way.

3. Size the VM
--------------

::

    GRAMINE_CPU_NUM=2 GRAMINE_RAM_SIZE=4G gramine-vm helloworld

- ``GRAMINE_CPU_NUM``: vCPUs, default 1.
- ``GRAMINE_RAM_SIZE``: guest memory. x86_64: default ``8G``, at least ``4G`` (with ``2G`` or
  ``3G`` QEMU refuses to start: ``total memory for NUMA nodes ... should equal RAM size``).
  AArch64: default ``2G``, at least ``1G``. A manifest's ``sgx.enclave_size`` or
  ``tdx.enclave_size`` takes precedence.

4. Processes: behind a key
--------------------------

By default the guest runs one process: ``fork`` fails with ``Function not implemented``. The
experimental key ``sys.experimental__multiprocess = true`` enables ``fork``, ``vfork``,
``execve``, pipes, signals and shared memory between processes. For example, in
``CI-Examples/bash`` add the key to ``manifest.template`` and rebuild::

    cd CI-Examples/bash
    echo 'sys.experimental__multiprocess = true' >> manifest.template
    make
    gramine-vm bash -c 'ls / | wc -l'          # prints a number; each child reports its exit
    git checkout manifest.template             # undo the change

On AArch64 the key is newer and prints that it is experimental: spawn, ``fork``, ``wait``,
pipes, signals and shared memory between processes work (bash, nginx with workers, redis
``BGSAVE``); a signal reaches user code only when a system call returns, and console and socket
waits are not yet interruptible. Status and limits:
`Documentation/devel/vm-multiproc/ <../devel/vm-multiproc/>`__ (x86_64) and
`Documentation/devel/vm-multiproc-aarch64/ <../devel/vm-multiproc-aarch64/>`__ (AArch64).

5. Networking
-------------

The guest has no TCP/IP stack of its own: TCP and UDP sockets are carried over vsock to a relay
that the launcher starts per VM (``vm-relay: cid=... listening ...``) and that opens the real
sockets on the host side. Outbound connections work by default; a server in the guest is reached
through an ``inbound`` forward in the manifest's ``[vm.net]`` table or ``GRAMINE_VM_INBOUND``. The
Redis example under the vm PAL, with its client on the host side::

    GRAMINE_RUNNER=gramine-vm GRAMINE_PREFIX=$HOME/.local/gramine-vm test/test-direct.sh redis

Keys and limits: the Networking section of `Documentation/devel/vm.md <../devel/vm.md>`__.

6. Regression tests
-------------------

::

    test/test-vm.sh libos              # the LibOS suite under gramine-vm, about 2 to 3 minutes

It ends with a pytest summary (failures with a known cause are marked xfail by
``libos/test/regression/vm_skip.toml``) and ``ALL VM SUITES PASSED``. ``test/test-vm.sh --help``
lists the other suites.

Troubleshooting
---------------

- ``/dev/kvm`` missing, or ``gramine-vm`` hangs or fails at once: the machine has no usable KVM
  (a cloud VM without nested virtualisation, or a different architecture).
- ``vhost-vsock: failed to open vhost device: Permission denied`` (AArch64), or socket calls fail
  with ``Address family not supported``: give your user access to ``/dev/vhost-vsock`` (as for
  ``/dev/kvm``) and load ``vhost_vsock``.
- AArch64 ``Mounting "file:<app>" ... failed: ENOENT``: the binary is missing or not a PIE; rebuild
  it with ``-fPIE -static-pie``.

Verified
--------

2026-10-15, ``main`` at ``9e28ea81``, container route, fresh clone and container:

- x86_64 host with KVM (Arch Linux host, Ubuntu 24.04 container): ``scripts/install.sh --vm``,
  hello world (``Hello, world``, ``[ VM exited with code 0 ]``, 1.1 s); ``GRAMINE_CPU_NUM=2`` with
  ``GRAMINE_RAM_SIZE`` ``4G`` and ``6G`` work, ``2G`` and ``3G`` fail as described; ``bash`` without
  the key fails at ``fork``, with the key prints ``10``; Redis through the relay: ``PASS``;
  ``test/test-vm.sh libos``: 125 passed, 94 skipped, 5 xfailed, 1 xpassed, 0 failed (2 min).
- AArch64 Ubuntu 24.04 host with KVM (NVIDIA GB10 machine), in the container: ``scripts/install.sh
  --vm``, hello world (0.8 s), ``GRAMINE_CPU_NUM=2 GRAMINE_RAM_SIZE=4G``; the multiprocess key
  prints the message above; Redis through the relay: ``PASS``; ``test/test-vm.sh libos``: 125
  passed, 96 skipped, 3 xfailed, 1 xpassed, 0 failed (2.5 min).

Not verified: the host route (its ``apt`` steps need sudo, which was not available).
