Quickstart: Containerized Userspace (``gramine-direct``)
=====================================================

``gramine-direct`` runs an unmodified Linux application as a container-like Linux process on top
of the Gramine LibOS. 

Status
------

Symbols as in `README.rst <../../README.rst>`__:

.. list-table::
   :header-rows: 1
   :widths: 32 17 17 17 17

   * - Configuration
     - Intel x86_64
     - AMD64
     - AArch64
     - RISC-V (RV64)
   * - ``gramine-direct``
     - ✅
     - ✅
     - ✅
     - ⚠️
   * - ``gramine-direct`` with redirected graphics
     - ⚠️
     - ⚠️
     - ⚠️
     - ❌

Requirements
------------

- Podman for the recommended container build, or Debian/Ubuntu for a host build.

1. Build
========

**Container (recommended**)**: 

``container/run.sh`` builds an Ubuntu
24.04 image with every build dependency and opens a shell in it, with the repository mounted at
``/root/gramine-multiarch``::

    cd <repo-home>
    ./container/run.sh                               # opens shell in the container (with cwd)
    ./scripts/install.sh                             # build and install into ~/.local/gramine
    . ~/.local/gramine/env.sh                        # PATH and PYTHONPATH for this shell

**Host (Debian/Ubuntu)**::

    cd <repo-home>
    ./scripts/dependencies.sh             # apt packages (uses sudo)
    ./scripts/install.sh                  # build and install into ~/.local/gramine
    . ~/.local/gramine/env.sh             # PATH and PYTHONPATH for this shell

2. Example: Hello World
=======================

::

    cd CI-Examples/helloworld
    make                                  # builds the program and renders helloworld.manifest
    gramine-direct helloworld

The output is ``Hello, world``. 


3. Regression Tests
===================

Execute the following commands (with ``env.sh`` sourced)::

    
    ./test/test-direct.sh --list                       # the suites
    ./test/test-direct.sh pal libos helloworld python  # a quick selection, under a minute
    ./test/test-direct.sh                              # everything: PAL, LibOS, 12 example suites

Each suite ends with a ``PASS`` or ``FAIL`` line. Logs are stored in ``/tmp/gramine-multiarch-tests``.

4. Troubleshooting
===================

- The environment variables are not set: run
  ``. ~/.local/gramine/env.sh`` (or ``scripts/install.sh --add-to-profile`` once).
- An application cannot find a file or library: the manifest mounts only what it names
  (``fs.mounts``); add the missing path, as the examples do. ``make DEBUG=1`` or
  ``loader.log_level = "debug"`` shows what the library OS looked for.