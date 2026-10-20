# Quickstart: AMD SEV Confidential Virtual Machine (`gramine-sev`)

`gramine-sev` runs a Gramine micro-VM as an AMD SEV-SNP guest. SEV-SNP protects guest
memory and registers from the host and supports hardware-signed attestation. The guest kernel is
loaded from an IGVM file measured at launch. See the [VM quickstart](vm.md) and the
dev docs: `sev/PLAN.md`.

## Status

Symbols as in [README.md](../../README.md):

| Configuration           | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|-------------------------|--------------|-------|---------|---------------|
| `gramine-sev` (SEV-SNP) | ➖           | ⚠️    | ➖      | ➖            |

Plain SEV/SEV-ES is also available: `./exec.sh --sev`, or `GRAMINE_SEV_MODE=sev` by hand.

## Requirements

All the requirements from [VM quickstart](vm.md) apply, and:

- An AMD EPYC host (Milan or later) with SEV-SNP enabled in the firmware and a host kernel with
  SNP support (`cat /sys/module/kvm_amd/parameters/sev_snp` prints `Y`).
  - Refer to [AMD's SEV-SNP GitHub repository](https://github.com/AMDESE/AMDSEV) for details on enabling SEV-SNP in the firmware and kernel.
- `/dev/sev` readable and writable by the user (add the user to the `sev` group on the host).
- `podman` for the container of `container/run.sh`. The SEV tools are built inside the container;
  nothing is installed on the host.

# 1. Build and install

The container is opened as in [direct.md](direct.md), section 1. Two steps build the SEV tools and
Gramine. QEMU 11.1.2 with `sev-snp-guest` and IGVM (including a local patch for plain SEV),
`snpguest`, `sevctl` and `sev-snp-measure` are built once into `sev-tools/`, next to the checkout
(it survives the recreation of the container):

    cd <repo-home>
    container/run.sh "container/sev/build-sev-tools.sh all"
    container/run.sh "scripts/install.sh --sev --graphics disabled"   # vm PAL + SEV PAL + gramine-sev

`--sev` builds the normal vm PAL as well, into `~/.local/gramine-vm` (the same prefix as
`--vm`); `gramine-sev` uses the QEMU of `sev-tools/` when `QEMU_PATH` is not set.
`--graphics disabled` leaves out the graphics paths, which the SEV guest does not use.
`container/run.sh` loads `~/.local/gramine-vm/env.sh` by itself, so the commands below need no
environment step.

# 2. Example: Hello World

In the container shell, `exec.sh` runs the example under SEV-SNP (`--sev-snp`) or under plain SEV
(`--sev`):

    cd CI-Examples/helloworld
    ./exec.sh --sev-snp
    ./exec.sh --sev

The script builds the example, sets `GRAMINE_SEV_MODE` to `snp` or `sev` and runs `gramine-sev`.
The output contains:

    gramine-sev: snp launch digest of .../sev/gramine-sev.igvm on 1 vCPUs: <96 hex digits>
    Hello, world
    [ VM exited with code 0 ]

The steps by hand:

    cd CI-Examples/helloworld
    make
    GRAMINE_SEV_MODE=snp gramine-sev helloworld

The digest depends on the PAL build and vCPU count; the attestation report must contain this value.

The guest's serial log (`GRAMINE_VM_SERIAL_LOG=<file>`) starts with `SEV: memory encryption active: SEV SEV-ES SEV-SNP (SEV_STATUS 0x7), C-bit 51`. The SEV PAL refuses to run without
encryption.

Results of 2026-10-20 on an AMD EPYC 7443P ([RESULTS.md](../../CI-Examples/RESULTS.md)):
`./exec.sh --sev` and `./exec.sh --sev-snp` pass for every example that supports them.

In a fresh container, `blender`, `mongodb`, `nodejs`, `openjdk` and `r` first stop with "cannot run here: missing on the host"; run `CI-Examples/install-all-prereqs.sh` once (or the `install-prereqs.sh` of that example, which the message names) and repeat the command.

Note the following:

- Set `GRAMINE_CPU_NUM` and `GRAMINE_RAM_SIZE` as for `gramine-vm`. The guest needs at least 6G
  (the launcher default and the scripts of `test/sev/` use 8G, which SEV pins at launch): `4G`
  no longer works, the guest stops with `Cannot initialize the multiprocess frame pool: Not enough memory`.
- Mode can be selected with `GRAMINE_SEV_MODE` or `sev.mode` in the manifest (`snp` by default, or `sev`).
- `GRAMINE_SEV_POLICY` or `sev.policy` sets the guest policy (default `0x30000` for SNP, with
  debugging disabled).

# 3. Attestation

Inside the guest, `/dev/attestation/attestation_type` reads `sev-snp`; write 64 bytes to
`/dev/attestation/user_report_data` and read `/dev/attestation/quote` (or `report`): the
1184-byte SNP attestation report with those bytes as its report data. `test/sev/sev_attest.c` does
this and writes the report to a host file:

    cd test/sev && make
    GRAMINE_SEV_MODE=snp gramine-sev sev_attest $(head -c 64 /dev/urandom | od -An -tx1 | tr -d ' \n') /tmp/report.bin

Check the report (in the container, where the SEV tools are): its signature against AMD's certificate chain (fetched from AMD's key
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

    GRAMINE_SEV_MODE=snp test/sev/test-sev.sh     # hello (1, 4 vCPUs), host-side memory check, vsock, attest
    test/sev/test-sev.sh                          # the same under plain SEV, with the launch measurement
    GRAMINE_VM_LAUNCHER=gramine-sev GRAMINE_SEV_MODE=snp GRAMINE_RAM_SIZE=8G test/vm/test-vm.sh libos
    test/sev/test-sev-all.sh                      # everything above, at 1, 2 and 4 vCPUs, plus examples

`GRAMINE_SEV_MODE=snp test/sev/test-sev.sh` runs under SNP, `test/sev/test-sev.sh` under plain SEV
(see the header of the script for the suites). On 2026-10-20 `test/sev/test-sev.sh` passed in both
modes ([RESULTS.md](../../CI-Examples/RESULTS.md)).

# 5. Troubleshooting

- `exec.sh` reports that `sev` cannot run: it names the reason. No `/dev/sev`: add the user to the
  `sev` group on the host, log in again; the container may have to be recreated to see the device (`container/run.sh down`
  discards the install inside it, then repeat section 1; not verified).
- `sev_snp` in `/sys/module/kvm_amd/parameters` is not `Y`: SNP is off in the firmware or the host
  kernel; plain SEV (`--sev`) may still work.
- `gramine-sev` cannot find QEMU: `container/sev/build-sev-tools.sh all` has not run, or
  `sev-tools/` is not next to the checkout. Set `SEV_TOOLS_DIR` or `QEMU_PATH`.
- `Cannot initialize the multiprocess frame pool: Not enough memory`: `GRAMINE_RAM_SIZE` is below
  6G; unset it or use `8G`.
- The signature check of an attestation report needs network access to AMD's key server.
