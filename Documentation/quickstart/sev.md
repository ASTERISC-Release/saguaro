# Quickstart: AMD SEV Confidential Virtual Machine (`gramine-sev`)

`gramine-sev` runs a Gramine micro-VM as an AMD SEV-SNP guest. SEV-SNP protects guest
memory and registers from the host and supports hardware-signed attestation. The guest kernel is
loaded from an IGVM file measured at launch. See the [VM quickstart](vm.md) and the
[SEV development record](../devel/sev/PLAN.md).

## Status

Symbols as in [README.md](../../README.md):

| Configuration           | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|-------------------------|--------------|-------|---------|---------------|
| `gramine-sev` (SEV-SNP) | ➖           | ⚠️    | ➖      | ➖            |

Note that plain SEV/SEV-ES is also available with `GRAMINE_SEV_MODE=sev`.

## Requirements

All the requirements from [VM quickstart](vm.md) apply, and:

- An AMD EPYC host (Milan or later) with SEV-SNP enabled in the firmware and a host kernel with
  SNP support (`cat /sys/module/kvm_amd/parameters/sev_snp` prints `Y`).
  - Refer to [AMD's SEV-SNP GitHub repository](https://github.com/AMDESE/AMDSEV) for details on enabling SEV-SNP in the firmware and kernel.
- `/dev/sev`readable and writable by your user (Add user to the `sev` group).
- `podman` for the container of `container/run.sh`.

# 1. Build

Build the SEV tools and Gramine in the container. QEMU 11.1.2 with `sev-snp-guest` and IGVM
(including a local patch for plain SEV), `snpguest`, `sevctl` and `sev-snp-measure` are built
in `sev-tools/` next to the checkout:

    cd <repo-home>
    ./container/run.sh "container/sev/build-sev-tools.sh all"
    ./container/run.sh                                            # shell in the container
    ./scripts/install.sh --sev --graphics disabled                # vm PAL + SEV PAL + gramine-sev
    . ~/.local/gramine-vm/env.sh

`--sev` builds the normal vm PAL as well; `gramine-sev` uses the QEMU of `sev-tools/` when
`QEMU_PATH` is not set.

# 2. Example: Hello World

    cd CI-Examples/helloworld
    make
    GRAMINE_RAM_SIZE=4G gramine-sev helloworld

The output contains:

    gramine-sev: snp launch digest of .../sev/gramine-sev.igvm on 1 vCPUs: <96 hex digits>
    Hello, world
    [ VM exited with code 0 ]

The digest depends on the PAL build and vCPU count; the attestation report must contain this value.

The guest's serial log (`GRAMINE_VM_SERIAL_LOG=<file>`) starts with `SEV: memory encryption active: SEV SEV-ES SEV-SNP (SEV_STATUS 0x7), C-bit 51`. The SEV PAL refuses to run without
encryption.

Note the following:

- Set `GRAMINE_CPU_NUM` and `GRAMINE_RAM_SIZE` as for `gramine-vm`.
- Mode can be selected with `GRAMINE_SEV_MODE` or `sev.mode` in the manifest (`snp` by default, or `sev`).
- `GRAMINE_SEV_POLICY` or `sev.policy` sets the guest policy (default `0x30000` for SNP, with
  debugging disabled).

# 3. Attestation

Inside the guest, `/dev/attestation/attestation_type` reads `sev-snp`; write 64 bytes to
`/dev/attestation/user_report_data` and read `/dev/attestation/quote` (or `report`): the
1184-byte SNP attestation report with those bytes as its report data. `test/sev/sev_attest.c` does
this and writes the report to a host file:

    cd test/sev && make
    gramine-sev sev_attest $(head -c 64 /dev/urandom | od -An -tx1 | tr -d ' \n') /tmp/report.bin

Check the report on the host: its signature against AMD's certificate chain (fetched from AMD's key
server) and its measurement against the IGVM file:

    export PATH=/root/sev-tools/prefix/bin:$PATH
    snpguest fetch ca pem /tmp/certs milan -e vcek
    snpguest fetch vcek pem /tmp/certs /tmp/report.bin
    python3 ~/.local/gramine-vm/lib/x86_64-linux-gnu/gramine/sev/gramine-sev-igvm.py verify-snp \
        ~/.local/gramine-vm/lib/x86_64-linux-gnu/gramine/sev/gramine-sev.igvm /tmp/report.bin \
        --vcpus 1 --certs /tmp/certs

which ends with `ok   signature by the VCEK` and `OK`. The measurement covers the PAL and its
boot state; the library OS, the application and its files come from the host through virtiofs and
are currently not measured.

# 4. Regression Tests

    GRAMINE_SEV_MODE=snp test/test-sev.sh     # hello (1, 4 vCPUs), host-side memory check, vsock, attest
    test/test-sev.sh                          # the same under plain SEV, with the launch measurement
    GRAMINE_VM_LAUNCHER=gramine-sev GRAMINE_SEV_MODE=snp GRAMINE_RAM_SIZE=4G test/test-vm.sh libos
    test/test-sev-all.sh                      # everything above, at 1, 2 and 4 vCPUs, plus examples
