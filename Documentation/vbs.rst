Gramine in Windows VBS enclaves
===============================

.. note::

   The ``vbs`` PAL is new and experimental. It has been run on one machine
   (Windows 11 Pro 25H2, build 26200, ARM64) with a test-signing certificate.
   The development notes, measurements and open items are in
   ``Documentation/devel/vbs-enclave/``.

The ``vbs`` PAL runs unmodified Linux AArch64 applications inside a Windows
*virtualization-based security* (VBS) enclave on ARM64 Windows 11. It follows
the design of the SGX PAL: a small untrusted host process on Windows creates the
enclave and serves its requests for files, sockets and time; the PAL, the LibOS
and the application run inside the enclave.

A VBS enclave cannot make memory executable at run time, so everything that
runs inside it is **packed into the signed enclave image when the image is
built**: the vbs PAL, the LibOS, the application, its shared libraries and its
data files (``vbs.packed_files``). The image is then linked, processed with
``veiid`` and signed on Windows. ``gramine-vbs`` does all of this from a
manifest.

How it fits together
--------------------

- **The image** (``<name>.dll``) holds a small Windows enclave runtime (the
  enclave entry points, thread start, exceptions, waits, attestation) and the
  packed Linux objects. Its enclave configuration (size, threads, debug flag,
  family and image IDs, SVN) comes from the manifest and is signed with it.
- **The host** (``pal_vbs_host.exe``) creates, loads and initialises the
  enclave, enters it, and serves its calls out: console, time, threads, host
  files below its *host root* folder, and Winsock sockets. Everything the host
  returns is untrusted, as in the SGX PAL.
- **The Linux side** of a build packs the objects and compiles the Windows
  enclave runtime and host with clang; **the Windows side** links, signs and
  runs. There are two ways to drive them: on **one Windows machine**, where
  the Linux side runs in WSL2 (``gramine-vbs.cmd``, see `Windows only (WSL)`_),
  or from a **Linux machine** that reaches the Windows machine over ssh
  (``scripts/vbs/gramine-vbs``).

Security properties and limits
------------------------------

- VBS enclaves are isolated from the Windows kernel and from administrators by
  the hypervisor and the Secure Kernel; they trust Hyper-V and the Secure
  Kernel. Unlike SGX there is no memory encryption against physical attacks.
- Files the manifest *allows* and every socket go through the untrusted host;
  *trusted* files are checked against their SHA-256; *packed* files are part
  of the signed and measured image.
- One process per enclave: ``fork``, ``vfork`` and ``execve`` into a new
  process are not supported (a design is sketched in
  ``Documentation/devel/vbs-enclave/PLAN.md``). ``execve`` within the process
  works.
- An enclave thread cannot be interrupted: a signal reaches it at its next
  system call (a busy loop without system calls does not see timer signals).
- Only test signing has been used. Production enclaves are signed through the
  VBS enclave profile of Azure Trusted Signing (not tried).
- Behaviour of the platform that affects the port (a register not preserved,
  an 8 KiB enclave thread stack, faults that hang) is described with
  reproducers in ``Documentation/devel/vbs-enclave/platform-issues.md``.

Installation
------------

Nothing in this section is done by the Gramine tools: each step changes the
machine and is the administrator's decision.

Windows machine
^^^^^^^^^^^^^^^

#. **ARM64 Windows 11** with **VBS running**. Check in PowerShell::

       (Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard `
           -ClassName Win32_DeviceGuard).VirtualizationBasedSecurityStatus

   ``2`` means running. VBS is turned on in *Windows Security* > *Device
   security* > *Core isolation* (memory integrity) or by group policy, and
   needs a restart.

#. **Test signing** for enclaves signed with a self-made certificate: as
   administrator, ``bcdedit /set testsigning on`` and a restart (``bcdedit``
   shows ``testsigning Yes``). With Secure Boot on, ``bcdedit`` may refuse the
   change ("protected by Secure Boot policy"); see Microsoft's documentation.

#. **Visual Studio** (2022 or later) with the *Desktop development with C++*
   workload and the **ARM64 build tools**, and the **Windows SDK 10.0.26100**
   or later (it provides ``vertdll.lib``, ``veiid.exe`` and ``signtool.exe``).
   ``link-sign.cmd`` finds Visual Studio with ``vswhere`` (or set
   ``VS_INSTALL_DIR``).

#. **A signing certificate** in the current user's store with the code
   signing, enclave signing and enclave author EKUs (OIDs
   ``1.3.6.1.5.5.7.3.3``, ``1.3.6.1.4.1.311.76.57.1.15`` and
   ``1.3.6.1.4.1.311.97.814040577.346743379.4783502.105532346``), as
   Microsoft's VBS enclave development guide describes. For example::

       New-SelfSignedCertificate -CertStoreLocation Cert:\CurrentUser\My `
           -Subject "CN=TheDefaultTestEnclaveCertName" -KeyAlgorithm RSA -KeyLength 2048 `
           -KeyUsage DigitalSignature -Type Custom -TextExtension @(
           "2.5.29.37={text}1.3.6.1.5.5.7.3.3,1.3.6.1.4.1.311.76.57.1.15,1.3.6.1.4.1.311.97.814040577.346743379.4783502.105532346")

   (the certificate used for the tests here already existed with this subject
   and these EKUs; the command above was not run on it). Give a different
   subject to the tools with ``--cert-name``.

#. For the ssh driver: the **OpenSSH server** of Windows, with key
   authentication for your user (the tools use ``ssh`` and ``scp``; the remote
   shell may be PowerShell or cmd).

Windows only (WSL)
^^^^^^^^^^^^^^^^^^

The whole build can run on the ARM64 Windows machine: its Linux half runs in a
**WSL2** distribution of the same machine (WSL uses the hypervisor that VBS
needs anyway). In addition to the Windows steps above (the ssh server is not
needed):

#. **WSL2 with an aarch64 Ubuntu 24.04** distribution (as administrator:
   ``wsl --install -d Ubuntu-24.04``, then a restart; on ARM64 Windows the
   distribution is aarch64). Another name is given with ``-Distro`` or
   ``GRAMINE_VBS_DISTRO``.

#. **A checkout of the repository on Windows** (any folder; a checkout with
   Windows line ends, Git's ``core.autocrlf=true``, works).

#. **The build tools inside the distribution**. ``gramine-vbs setup`` checks
   the Windows side and the distribution and prints what is missing; then
   either::

       scripts\vbs\gramine-vbs.cmd setup -Install   # no sudo: into ~/.local/share/gramine-vbs
       scripts\vbs\gramine-vbs.cmd setup -Apt       # sudo apt-get install (asks for the password)

   ``-Install`` downloads the same Ubuntu packages with ``apt-get download``
   and unpacks them into a folder of your home (about 290 MB, 7 s): bison, m4,
   pkgconf, meson, ninja, clang 18 and four Python modules. ``-Apt`` installs
   them system-wide: ``bison m4 pkgconf meson ninja-build python3-tomli
   python3-tomli-w python3-voluptuous python3-pyelftools clang``. Nothing else
   is ever installed or changed by the tools.

The first build copies the SDK and MSVC headers into the distribution (once
per version, 156 MB), builds the vbs glibc (about 2 minutes) and the vbs PAL
and LibOS (1 minute); see `Quick start on Windows`_.

Linux side
^^^^^^^^^^

#. **The vbs PAL and LibOS**, built on an **aarch64 Linux** machine (or in the
   repository's aarch64 build container)::

       meson setup build-vbs -Ddirect=disabled -Dvbs=enabled -Dlibc=none \
           -Dtests=disabled -Dbuildtype=release -Dgraphics=disabled
       ninja -C build-vbs

#. **The vbs glibc** (glibc 2.39 built with ``-ffixed-x18`` and 4 KiB pages),
   the sysroot that ``scripts/vbs/vbs-gcc`` compiles against:
   ``scripts/vbs/build-glibc.sh`` (into ``build-vbs-glibc``).

#. On the machine that runs ``gramine-vbs build`` (any architecture): Python
   3.11 or later, ``clang`` with the ``aarch64-pc-windows-msvc`` target, and
   ssh access to the Windows machine. If this is not the aarch64 machine, copy
   ``build-vbs`` (or just ``libpal.so``, ``libsysdb.so`` and ``vdso.so``) and
   the files to pack, and point ``--runtime`` at them.

#. **The Windows SDK and MSVC headers** for clang, copied once from the
   Windows machine::

       scripts/vbs/gramine-vbs setup-headers --windows user@windows-host

   (into ``~/.cache/gramine-vbs/winhdr``; about 160 MB).

Quick start on Windows
----------------------

In PowerShell or ``cmd``, in the checkout::

    scripts\vbs\gramine-vbs.cmd setup
    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\helloworld\helloworld.manifest

The first ``run`` copies the checkout into the distribution
(``~/.cache/gramine-vbs/checkouts/<name>-<hash>``), builds the vbs glibc, the
PAL and LibOS, compiles the program (the manifest's ``vbs.build``), packs,
compiles the Windows objects with clang in WSL, then links, signs and runs it
here: 3.5 minutes measured; a ``-Rebuild`` later takes 11 s, a plain ``run``
of the signed image 0.4 s. The output::

    Hello, world from a VBS enclave (pid 1, 0 arguments)

The dynamically linked example (``CI-Examples\vbs\dynhello``) packs the
program with the dynamic loader, ``libc.so.6`` and ``libm.so.6``::

    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\dynhello\dynhello.manifest

Python (``CI-Examples\vbs\python``): the manifest's ``vbs.build`` builds
CPython 3.12.7 for the enclave in the distribution the first time
(``scripts/vbs/build-python.sh --if-needed``: downloaded from python.org and
checked against its SHA-256, built against the vbs glibc in 78 s, kept in
``build-vbs-python`` of the WSL copy and reused afterwards)::

    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\python\python.manifest

The first run took 100 s, a rebuild with the cached CPython 21 s, a run of the
signed image 1.9 s.

A TCP server (``CI-Examples\vbs\tcpserver``) listens on 127.0.0.1:18080
through the host's Winsock sockets and answers three clients::

    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\tcpserver\tcpserver.manifest
    # in a second window:
    CI-Examples\vbs\tcpserver\client.ps1 -Count 3

**Adding an application** takes a manifest with ``vbs.packed_files`` (what goes
into the image) and ``vbs.build`` (how its files are built in the
distribution, with the enclave compilers ``$VBS_CC_STATIC``,
``$VBS_CC_DYNAMIC`` and ``$VBS_GCC`` against the vbs glibc), as in the
examples; see :ref:`vbs-syntax`. A manifest outside the checkout works too;
its folder is copied into the distribution (``~/.cache/gramine-vbs/apps``), so
its ``vbs.packed_files`` must stay inside that folder or point into the
checkout's build folders through ``vbs.build``.

Quick start on Linux
--------------------

Settings for the examples (or give the matching options)::

    export GRAMINE_VBS_WINDOWS=user@windows-host
    export GRAMINE_VBS_RUNTIME=<repository>/build-vbs   # libpal.so, libsysdb.so, vdso.so
    # optional: GRAMINE_VBS_WINDOWS_DIR (default %USERPROFILE%\gramine-vbs\<name>),
    #           GRAMINE_VBS_HEADERS, GRAMINE_VBS_CERT_NAME, GRAMINE_VBS_SSH_OPTS

Hello world (``CI-Examples/vbs/helloworld``; its ``vbs.build`` compiles the
program against ``build-vbs-glibc``, so run this on the aarch64 machine or
build the program there first)::

    cd CI-Examples/vbs/helloworld
    ../../../scripts/vbs/gramine-vbs run helloworld.manifest

The first ``run`` builds the image (pack, compile, upload, link, sign: 17 s
measured), later runs start the signed image (0.9 s). The output::

    Hello, world from a VBS enclave (pid 1, 0 arguments)

Python (``CI-Examples/vbs/python``; its ``vbs.build`` builds CPython 3.12.7
once with ``scripts/vbs/build-python.sh --if-needed`` into
``build-vbs-python``, which needs the aarch64 machine)::

    cd CI-Examples/vbs/python
    ../../../scripts/vbs/gramine-vbs run python.manifest

The manifest packs the interpreter, the standard library (58 extension modules
and 523 other files without tests and Tk) and the vbs glibc; the first run took
24.6 s including the build, a later one 1.9 s.

``gramine-vbs``
---------------

::

    gramine-vbs build  <manifest> [-o DIR] [--no-sign]
    gramine-vbs sign   <manifest|DIR>
    gramine-vbs run    <manifest|DIR> [--threads N] [--root WINDIR] [--rebuild] [-v] [-- ARGS]
    gramine-vbs verify <manifest|DIR> <report on Windows> [verifier options]
    gramine-vbs setup-headers

- ``build`` packs the runtime, the manifest and ``vbs.packed_files`` and
  compiles the Windows objects into ``<name>.vbs/`` next to the manifest; with
  a Windows machine configured it also signs.
- ``sign`` uploads the objects to the Windows folder and runs
  ``link-sign.cmd`` there: ``<name>.dll`` (signed) and ``pal_vbs_host.exe``.
- ``run`` builds first when the image is missing or the manifest changed, then
  runs ``pal_vbs_host.exe <name>.dll <threads> boot <root> ARGS``; the host root
  defaults to ``root`` in the Windows folder, and the directories of
  ``vbs.allowed_files`` are created below it. The exit code is the
  application's. Arguments reach the application only with
  ``loader.insecure__use_cmdline_argv = true``.
- ``verify`` checks an attestation report (see below).

Options: ``--runtime``, ``--headers``, ``--windows``, ``--windows-dir``,
``--cert-name`` (and the environment variables of the same names, see
``gramine-vbs --help``).

On Windows, ``scripts\vbs\gramine-vbs.cmd`` (which runs ``gramine-vbs.ps1``
with Windows PowerShell) has the same commands, with the Linux side in WSL::

    gramine-vbs.cmd setup  [-Install | -Apt]
    gramine-vbs.cmd build  <manifest> [-Out DIR] [-NoSign]
    gramine-vbs.cmd sign   <manifest|DIR>
    gramine-vbs.cmd run    <manifest|DIR> [-Threads N] [-Root DIR] [-Rebuild] [-ShowHost] [-- ARGS]
    gramine-vbs.cmd verify <manifest|DIR> -Report FILE [-- verifier options]

with ``-Distro NAME`` and ``-CertName SUBJECT`` for all of them. The build
folder ``<name>.vbs`` is next to the manifest and holds the objects, the
signed ``<name>.dll``, ``pal_vbs_host.exe``, ``objs-info.txt`` (the clang,
gcc and header versions and the object hashes; the objects are
reproducible), and ``root`` (the default host root). ``run`` rebuilds when a
file of the checkout (or of the manifest's folder outside it) changed since the
image was built, by name, size and modification time, or with ``-Rebuild``;
files deleted on Windows are deleted from the WSL copy too. Inputs reach WSL in
environment variables, so folders with spaces or non-ASCII names work.

Objects that ``gramine-vbs build --no-sign`` prepared on Linux (copy
``<name>.vbs/obj`` with ``link-sign.cmd`` and ``pal_vbs_enclave.def``) are
signed, run and verified with the earlier form::

    .\gramine-vbs.ps1 sign   -Dir <folder> -Name hello
    .\gramine-vbs.ps1 run    -Dir <folder> -Name hello
    .\gramine-vbs.ps1 verify -Dir <folder> -Name hello -Report <file> -- --same-image

Manifest keys: :ref:`vbs-syntax`.

Attestation
-----------

Inside the enclave, Gramine's usual interface (:doc:`attestation`) works:

- ``/dev/attestation/attestation_type`` reads ``vbs``;
- ``/dev/attestation/user_report_data`` takes 64 bytes of user data;
- ``/dev/attestation/report`` and ``/dev/attestation/quote`` return the
  **VBS attestation report package** for that user data (the same bytes: the
  report is already signed by the platform, so there is no separate quote).
  There is no ``target_info``.

**What the report holds.** A 24-byte header (version 1, signature scheme 1 =
RSA-PSS with SHA-256, the sizes), the signed statement and a 256-byte
signature. The signed statement is the report: the **64 bytes of user data**,
the **enclave identity** -- unique ID (the image's measurement, which covers
every packed file), author ID (derived from the signing certificate: two different images signed
with the same certificate had the same author ID), family ID and
image ID (``vbs.family_id``, ``vbs.image_id``), enclave SVN
(``vbs.isv_svn``), Secure Kernel and platform SVNs, flags (the debug flag),
signing level, enclave type, and the owner ID (chosen by the host when it
creates the enclave: untrusted) -- and one entry per module loaded in the
enclave (the image and the Windows enclave DLLs, with their IDs and SVNs). A
report is about 1.2 KB.

**Verifying a report.** ``gramine-vbs verify`` (or ``pal_vbs_host.exe
<image> 1 verify <report> [options]`` on Windows) checks:

#. the package's structure;
#. **the platform signature**: a fresh enclave of the given image calls
   ``EnclaveVerifyAttestationReport``, which checks the signature with the
   platform of the machine it runs on;
#. what is asked: ``--user-data HEX`` or ``--user-data-file F`` (the user data,
   zero-padded to 64 bytes), ``--same-image`` (unique ID and author ID equal
   to those of the verifying image), ``--unique-id HEX`` and ``--author-id
   HEX`` (pinned values), ``--min-svn N``, and ``--allow-debug`` (a debug
   enclave is refused otherwise).

It prints the report's fields and exits with 0 when every check passed. The
example ``CI-Examples/vbs/attestation`` gets a report with user data from the
command line::

    gramine-vbs run attest.manifest -- <64 hex digits>
    gramine-vbs verify attest.vbs <windows folder>\root\data\out\report.bin \
        --user-data <64 hex digits> --same-image --min-svn 1

and on Windows alone::

    gramine-vbs.cmd run attest.manifest -- <64 hex digits>
    gramine-vbs.cmd verify attest.vbs -Report attest.vbs\root\data\out\report.bin -- ^
        --user-data <64 hex digits> --same-image --min-svn 1

**What this establishes, and what it does not.** A successful local check
shows that the report was produced on *this* machine by a VBS enclave with
that identity (code measurement, signer, IDs, SVN, debug flag) and that the
enclave chose that user data; a relying party makes it fresh by putting a
nonce (or a key's hash) in the user data. It does not show anything to a
party on another machine: that needs **Microsoft Azure Attestation**, which
documents a VBS attestation flow that takes the report together with the
machine's TPM-measured boot log (proving that VBS and Secure Boot were on).
That flow is not implemented here and was not tried. The owner ID is the
host's choice and proves nothing.

The test ``pal/src/host/vbs/test/attest/test-attest.ps1`` checks the
positive case and the rejection of a changed byte of the user data, of the
identity and of the signature, a changed size field, other user data, another
pinned unique ID, a too-low SVN and a report verified as another image.

Sealing (``EnclaveSealData``) is not exposed yet.
