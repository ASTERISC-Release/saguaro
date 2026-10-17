Quickstart: ``gramine-direct`` on x86_64 and AArch64
=====================================================

``gramine-direct`` runs an unmodified Linux application as an ordinary Linux process on top of the
library OS. It needs no virtualisation and works on a host or in a container. It is **not a
sandbox**: use it to try Gramine, to develop, and to compare architectures. Full instructions:
`INSTALL.rst <../../INSTALL.rst>`__.

Status (as in `README.rst <../../README.rst>`__): Intel x86_64 ✅, AMD64 ✅, AArch64 ✅. RISC-V:
`riscv.rst <riscv.rst>`__.

1. Get the code
---------------

::

    git clone https://github.com/adilahmad17/gramine-multiarch.git
    cd gramine-multiarch

2. Build and install
--------------------

**In a container (recommended, needs only** ``podman`` **)**: ``container/run.sh`` builds an Ubuntu
24.04 image with every build dependency and opens a shell in it, with the repository mounted at
``/root/gramine-multiarch``::

    container/run.sh                   # shell in the container; the rest runs inside
    scripts/install.sh                 # build and install into ~/.local/gramine
    . ~/.local/gramine/env.sh          # PATH and PYTHONPATH for this shell

**On the host (Debian/Ubuntu)**::

    scripts/dependencies.sh            # apt packages (uses sudo)
    scripts/install.sh
    . ~/.local/gramine/env.sh

``scripts/install.sh --help`` lists the options. On x86_64 the default build includes a patched
glibc, which makes system calls cheaper; on AArch64 the application uses the host's glibc.

3. Hello world
--------------

::

    cd CI-Examples/helloworld
    make                               # builds the program and renders helloworld.manifest
    gramine-direct helloworld

The output is ``Hello, world``. On AArch64 a line ``Emulating a raw system/supervisor call ...``
comes before it; it is a performance hint and harmless.

What happened: ``make`` rendered ``helloworld.manifest.template`` into ``helloworld.manifest``
(what the program may see: its binary, the libraries it mounts, its environment), and
``gramine-direct helloworld`` started the library OS with that manifest. Every manifest key is
described in `manifest-syntax.rst <../manifest-syntax.rst>`__; ``CI-Examples/`` holds more
applications (``bash``, ``python``, ``redis``, ``nginx``, ...), each with a ``README.md``.

4. Run the regression tests
---------------------------

From the repository root (in the container or with the install's ``env.sh`` sourced)::

    test/test-direct.sh --list                       # the suites
    test/test-direct.sh pal libos helloworld python  # a quick selection, under a minute
    test/test-direct.sh                              # everything: PAL, LibOS, 12 example suites

Each suite ends with a ``PASS`` or ``FAIL`` line; the logs are in ``/tmp/gramine-multiarch-tests``.
Some example suites build their application first (``redis``, ``nginx``, ...), which takes
longer the first time. More ways to test: `Documentation/devel/multiarch.md
<../devel/multiarch.md>`__.

5. Troubleshooting
------------------

- ``PAL failed setting personality failed: Function not implemented``: the process runs under a
  seccomp profile that blocks ``personality()``, typically a container started by hand. Use
  ``container/run.sh`` or ``--security-opt seccomp=unconfined``.
- ``make: gramine-manifest: No such file or directory`` (or the Python module ``graminelibos``
  is missing): the install's environment is not loaded in this shell; run
  ``. ~/.local/gramine/env.sh`` (or ``scripts/install.sh --add-to-profile`` once).
- An application cannot find a file or library: the manifest mounts only what it names
  (``fs.mounts``); add the missing path, as the examples do. ``make DEBUG=1`` or
  ``loader.log_level = "debug"`` shows what the library OS looked for.

Next: run the same application in a microVM (`vm.rst <vm.rst>`__), or with the host's GPU
(`graphics-direct.rst <graphics-direct.rst>`__).

Verified
--------

2026-10-15, at commit ``9e28ea81`` of ``main``, from a fresh clone and a fresh container
(``container/run.sh``):

- x86_64 host (Arch Linux, kernel 7.2) in the Ubuntu 24.04 container: ``scripts/install.sh``
  (1 min 21 s), hello world (``Hello, world``); ``test/test-direct.sh pal libos helloworld python``:
  PAL 32 passed / 4 skipped, LibOS 155 passed / 37 failed / 34 skipped (a startup fault that
  depended on the length of the install path, not on the libc; fixed the same day and verified
  since by CI (LibOS 190 passed / 36 skipped) and ``test/direct-prefix-sweep.sh`` at every path
  length, see `vm-multiproc/TESTING.md <../devel/vm-multiproc/TESTING.md>`__), helloworld and
  python pass; ``bash`` and ``busybox`` pass. A second install with
  ``scripts/install.sh --libc none`` (its prefix fell outside the failing length range): LibOS
  192 passed / 34 skipped. Without ``env.sh``, ``make`` fails as described under
  Troubleshooting.
- AArch64 Ubuntu 24.04 host with an NVIDIA GB10, in the container: ``scripts/install.sh``, hello
  world; ``test/test-direct.sh pal libos helloworld python``: PAL 28 / 8 skipped, LibOS 190 passed /
  36 skipped, helloworld and python pass (python reports numpy/scipy as missing in the container).

Not verified: the host route (``scripts/dependencies.sh`` needs sudo, which was not available;
``DRY_RUN=1 scripts/dependencies.sh`` in the container found every package present), the full
``test/test-direct.sh``, and the ``https`` clone URL (the clone was made over ssh).
