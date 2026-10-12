Installing
==========

This tree builds and runs on Debian/Ubuntu (24.04 is the tested baseline) for two
architectures, **x86_64** and **aarch64**, and has two ways to run an application (the experimental
**riscv64** port of the direct PAL is cross-built instead; see `RISC-V (cross-build and test guest)`_):

- ``gramine-direct`` -- the *direct* PAL: the library OS runs as an ordinary Linux process.
  Needs only the build tools; works anywhere, including inside a container.
- ``gramine-vm`` -- the *vm* PAL: the library OS and a small kernel boot as a unikernel in a QEMU
  guest. Needs a **native** machine of the same architecture with KVM (``/dev/kvm``), a stock
  ``qemu-system-*``, ``virtiofsd`` and, on aarch64, ``dtc``. On aarch64 it is experimental.

Pick one of the two routes below: install on the host (`Installing on the host`_) or use the
supplied containers (`Installing in a container`_). The container route is the quickest way to get the
``vm`` PAL, because the image already has QEMU, ``virtiofsd`` and the toolchain.

Installing on the host
----------------------

Direct PAL
^^^^^^^^^^

From the repository root::

    scripts/dependencies.sh           # host packages (apt, uses sudo); --build-only for just the build
    scripts/install.sh                # build and install into ~/.local/gramine
    . ~/.local/gramine/env.sh         # put the install on PATH / PYTHONPATH (this shell)

``scripts/install.sh`` is safe to re-run. Useful options (``scripts/install.sh --help`` lists all):

==========================  ==========================================================================
``--prefix DIR``            install prefix (default ``~/.local/gramine``; no root needed)
``--build-dir DIR``         meson build directory (default ``build-<arch>``)
``--deps``                  run ``scripts/dependencies.sh --build-only`` first
``--add-to-profile``        source ``<prefix>/env.sh`` from ``~/.bashrc`` so new shells have Gramine
``--graphics disabled``     leave out the Tirith graphics host-redirection paths
``-j N``                    parallel build jobs (default: all cores)
``--run-tests [suite...]``  run ``test/test-direct.sh`` afterwards (naming suites implies it)
==========================  ==========================================================================

Check the install and run an example::

    test/test-direct.sh               # PAL + LibOS regression tests and the example applications
    test/test-direct.sh libos         # only the named suites (``--list`` shows them)

    cd CI-Examples/helloworld
    make
    gramine-direct helloworld

Virtual-machine PAL (``gramine-vm``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Install the QEMU side, then build with ``--vm``.

aarch64::

    sudo apt install qemu-system-arm virtiofsd device-tree-compiler    # stock Ubuntu QEMU (8.2.x)
    scripts/dependencies.sh --build-only
    scripts/install.sh --vm           # into ~/.local/gramine-vm, build dir build-vm-aarch64
    . ~/.local/gramine-vm/env.sh
    test/test-vm.sh                   # LibOS suite under gramine-vm, example apps, hello soak test

On aarch64 ``--vm`` builds *only* the vm PAL (no direct PAL, SGX or TDX), forces
``-Dgraphics=disabled`` (the graphics redirection needs the QEMU fork's host agent, which does not exist
for aarch64) and builds the test programs as PIE, because the guest maps RAM at ``0x40000000``. Use stock
distribution QEMU; the repository's QEMU fork does not build for aarch64 and is not needed. The aarch64
VM PAL runs single-process applications (no ``fork``/``exec``), with vsock but no TCP/IP; see
``Documentation/devel/vm.md`` and ``Documentation/TODO.md`` for the exact limits.

x86_64::

    sudo apt install qemu-system-x86 virtiofsd lsof python3-psutil
    scripts/install.sh --vm --graphics disabled [--tdx] [--sgx]
    sudo ln -sf /usr/libexec/virtiofsd /usr/local/bin/virtiofsd    # if `command -v virtiofsd` finds nothing

The x86_64 vm PAL is inherited from upstream gramine-tdx. ``gramine-vm`` runs the QEMU named by
``QEMU_PATH`` and otherwise a stock ``qemu-system-x86_64`` from ``PATH``. TDX and the graphics
redirection need a QEMU fork that is kept outside this repository; for graphics see
``Documentation/devel/graphics/running.md``. With graphics enabled (the default), ``--vm`` also builds
the ``libgraphics.so`` preload library and therefore needs the Mesa, X and SDL2 development packages;
pass ``--graphics disabled``, as above, to build without them.

A ``--vm`` build, and any aarch64 build, without ``--sgx`` or ``--tdx`` builds **no libc** (``-Dlibc=none``; the build
is much shorter): applications run in the guest on the host's glibc, which the VM serves from the host's
``/``. ``gramine.runtimedir()`` in the manifests then names the directory with the host's ``libc.so.6`` and
dynamic loader, found when the manifest is rendered; ``GRAMINE_HOST_LIBC_DIR`` overrides the search.
``--libc glibc`` builds and uses the bundled glibc instead (SGX needs it, and so do builds for other
architectures by default). One consequence on x86_64: the direct PAL of a ``--vm`` build runs on an unpatched libc, so
each syscall takes the seccomp trap (about 0.85 us); on aarch64 the direct PAL does not use the libc patch.
For a direct PAL with the patched glibc on x86_64, build without ``--vm``, in a build directory of its own.
An x86_64 direct build without ``--vm`` and a riscv64 build keep the bundled glibc by default.

Running an application under the VM PAL is described in ``Documentation/devel/vm.md``: the manifest's
loader entrypoint must point at the VM ``libsysdb.so`` (``make`` against the VM install does this) and
the binary must be a PIE on aarch64, then ``gramine-vm <application>``. A kernel panic or guest exit is
reported as ``[ VM exited with code N ]``.

Installing in a container
-------------------------

``container/run.sh`` is the single entry point. It detects the machine's architecture, builds the matching
image if it is missing or its Containerfile changed, creates and starts a long-lived container, and opens a
shell in it. The repository is mounted at ``/root/gramine-multiarch``; builds and install prefixes live
inside the container's home. Requirements: ``podman``, and for ``gramine-vm`` a native host of the same
architecture with ``/dev/kvm``.

::

    container/run.sh                      # shell in the container for this machine
    container/run.sh '<command>'          # one-shot command (no tty)
    container/run.sh --rebuild            # force an image rebuild and container recreation
    container/run.sh down                 # stop and remove the container (the image is kept)

Typical session (aarch64 shown; on x86_64 the container is ``gramine-vm-x86_64``)::

    container/run.sh                              # shell inside
    scripts/install.sh pal libos                  # direct PAL: build, install, run the PAL + LibOS suites
    scripts/install.sh --vm libos examples hello  # vm PAL: build, install, run the vm test battery

or as one-shot commands from the host::

    container/run.sh 'scripts/install.sh --vm libos examples hello'

Notes:

- Images are ``gramine-vm-aarch64`` (``container/Containerfile.vm-aarch64``) and ``gramine-vm-x86_64``
  (``container/Containerfile.vm-x86_64``); set ``CONTAINER_NAME`` / ``IMAGE_NAME`` to use other names.
- The container runs with ``--cap-add ALL`` (virtiofsd serves the container's ``/`` as the guest's ``/``)
  and ``seccomp=unconfined`` (the direct PAL calls ``personality()``, which the default profile blocks).
- A changed Containerfile triggers a rebuild, and a container created from an older image is recreated;
  that discards the container's ``/tmp`` (files in the repository survive).

RISC-V (cross-build and test guest)
-----------------------------------

The RISC-V (RV64) port covers the direct PAL and is experimental. It is cross-built on an x86_64 or
AArch64 machine and tested in a RISC-V Linux guest under ``qemu-system-riscv64``; user-mode emulation
cannot run it, because it has no seccomp. Everything runs unprivileged, in a container of its own::

    container/run.sh --target riscv64                       # shell in the cross container
    scripts/install.sh --target riscv64 --prefix "$PWD/build-riscv64/prefix"   # build and install
    scripts/riscv64/guest.sh start                          # boot the guest (first boot installs packages)
    scripts/riscv64/run-suite.sh pal                        # PAL regression suite in the guest
    scripts/riscv64/run-suite.sh libos                      # LibOS regression suite in the guest
    scripts/riscv64/run-examples.sh helloworld bash redis   # example suites, built in the guest
    scripts/riscv64/guest.sh stop

The guest sees the repository at the same path as the container does, so it runs the cross-built
binaries directly; this is why the install prefix above lies inside the repository. The suites are run from the guest's own disk, not from that shared directory.
Design, status and known limits are in ``Documentation/devel/riscv/PLAN.md``.

Building by hand
----------------

``scripts/install.sh`` wraps ``meson``; the equivalent for the direct PAL is::

    meson setup build -Dbuildtype=release -Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dtdx=disabled \
        -Dtests=enabled -Dlibc=glibc --prefix="$HOME/.local/gramine"
    ninja -C build install

and for the aarch64 vm PAL::

    meson setup build-vm-aarch64 -Dbuildtype=release -Ddirect=disabled -Dvm=enabled -Dsgx=disabled \
        -Dtdx=disabled -Dgraphics=disabled -Dtests=enabled -Dtests_pie=true -Dlibc=glibc \
        --prefix="$HOME/.local/gramine-vm"
    ninja -C build-vm-aarch64 install

The available options are in ``meson_options.txt``; ``-Dlibc=none`` skips building glibc (see the x86_64 VM
PAL above).

Troubleshooting
---------------

- ``PAL failed setting personality failed: Function not implemented`` -- the direct PAL is running under a
  seccomp profile that blocks ``personality()`` (typically a default container). Run it natively or in a
  container with ``--security-opt seccomp=unconfined`` (``container/run.sh`` does this).
- ``gramine-vm`` does nothing useful / ``/dev/kvm`` missing -- the vm PAL needs hardware virtualisation on a
  machine of the *same* architecture; emulation (TCG) is not supported.
- ``[ VM exited with code 134 ]`` -- the guest kernel panicked; the lines above it hold the register dump.
- aarch64 ``Mounting "file:<app>" ... failed: ENOENT`` -- the application binary is not built or is not a
  PIE; rebuild it as ``-fPIE -static-pie`` (the example runner ``tools/run-vm-aarch64-examples.sh``
  shows how).
