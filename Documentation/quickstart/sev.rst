Quickstart: ``gramine-sev`` on AMD SEV-SNP
==========================================

``gramine-sev`` runs the micro-VM of ``gramine-vm`` (`vm.rst <vm.rst>`__) as an AMD SEV-SNP
guest: the host can neither read nor change the guest's memory or registers, and the guest can
prove what it runs with an attestation report signed by the AMD chip. The guest kernel boots from
an IGVM file whose every page is measured at launch; ``gramine-sev`` prints the launch digest that
the attestation report must carry. Design and status: `Documentation/devel/sev/
<../devel/sev/PLAN.md>`__.

Status (as in `README.rst <../../README.rst>`__): AMD64 ⚠️ (experimental), SEV-SNP on an EPYC 7443P
(Milan) host; plain SEV works too (``GRAMINE_SEV_MODE=sev``). Not supported yet: SEV-ES alone (the
development host cannot launch it), the graphics redirection, a trusted-files check on the
host-supplied files (virtiofs: SNP protects the guest's memory, not the files it reads).

Requirements
------------

- An AMD EPYC host (Milan or later) with SEV-SNP enabled in the firmware and a host kernel with
  SNP support (``cat /sys/module/kvm_amd/parameters/sev_snp`` prints ``Y``).
- ``/dev/kvm``, ``/dev/sev`` and ``/dev/vhost-vsock`` readable and writable by your user (``/dev/sev``
  usually through a group such as ``sev``: ask the administrator).
- ``podman`` for the container of ``container/run.sh``. Root is not needed.

1. Build the SEV tools and Gramine (in the container)
------------------------------------------------------

QEMU with ``sev-snp-guest`` and IGVM (11.1.2, with one local patch for plain SEV), ``snpguest``,
``sevctl`` and ``sev-snp-measure`` are built into ``sev-tools/`` next to the checkout, outside the
image::

    container/run.sh "container/sev/build-sev-tools.sh all"
    container/run.sh                                            # shell in the container
    scripts/install.sh --sev --graphics disabled                # vm PAL + SEV PAL + gramine-sev
    . ~/.local/gramine-vm/env.sh

``--sev`` builds the normal vm PAL as well; ``gramine-sev`` uses the QEMU of ``sev-tools/`` when
``QEMU_PATH`` is not set.

2. Hello world
--------------

::

    cd CI-Examples/helloworld
    make
    GRAMINE_RAM_SIZE=4G gramine-sev helloworld

The output contains::

    gramine-sev: snp launch digest of .../sev/gramine-sev.igvm on 1 vCPUs: 2f5e5299...df747148
    Hello, world
    [ VM exited with code 0 ]

The guest's serial log (``GRAMINE_VM_SERIAL_LOG=<file>``) starts with ``SEV: memory encryption
active: SEV SEV-ES SEV-SNP (SEV_STATUS 0x7), C-bit 51``. The SEV PAL refuses to run without
encryption.

Settings: ``GRAMINE_CPU_NUM`` and ``GRAMINE_RAM_SIZE`` as for ``gramine-vm``;
``GRAMINE_SEV_MODE`` (or ``sev.mode`` in the manifest) ``snp`` (default) or ``sev``;
``GRAMINE_SEV_POLICY`` (or ``sev.policy``), default ``0x30000`` for SNP (debugging off).

3. Attestation
--------------

Inside the guest, ``/dev/attestation/attestation_type`` reads ``sev-snp``; write 64 bytes to
``/dev/attestation/user_report_data`` and read ``/dev/attestation/quote`` (or ``report``): the
1184-byte SNP attestation report with those bytes as its report data. ``test/sev/sev_attest.c`` does
this and writes the report to a host file::

    cd test/sev && make
    gramine-sev sev_attest $(head -c 64 /dev/urandom | od -An -tx1 | tr -d ' \n') /tmp/report.bin

Check the report on the host: its signature against AMD's certificate chain (fetched from AMD's key
server) and its measurement against the IGVM file::

    export PATH=/root/sev-tools/prefix/bin:$PATH
    snpguest fetch ca pem /tmp/certs milan -e vcek
    snpguest fetch vcek pem /tmp/certs /tmp/report.bin
    python3 ~/.local/gramine-vm/lib/x86_64-linux-gnu/gramine/sev/gramine-sev-igvm.py verify-snp \
        ~/.local/gramine-vm/lib/x86_64-linux-gnu/gramine/sev/gramine-sev.igvm /tmp/report.bin \
        --vcpus 1 --certs /tmp/certs

which ends with ``ok   signature by the VCEK`` and ``OK``. The measurement covers the PAL and its
boot state; the library OS, the application and its files come from the host through virtiofs and
are not measured (no trusted-files check yet).

4. Tests
--------

::

    GRAMINE_SEV_MODE=snp test/test-sev.sh     # hello (1, 4 vCPUs), host-side memory check, vsock, attest
    test/test-sev.sh                          # the same under plain SEV, with the launch measurement
    GRAMINE_VM_LAUNCHER=gramine-sev GRAMINE_SEV_MODE=snp GRAMINE_RAM_SIZE=4G test/test-vm.sh libos
    test/test-sev-all.sh                      # everything above, at 1, 2 and 4 vCPUs, plus examples

What was verified
-----------------

On 2026-10-16, on the development host of `Documentation/devel/sev/TESTING.md
<../devel/sev/TESTING.md>`__ (AMD EPYC 7443P, kernel ``6.11.0-snp-host``, SEV firmware 1.58): the
steps above, with the outputs shown; the LibOS suite under SNP passes as without SEV (one known
failure, ``test_020_cpuinfo``, on AMD hosts).
