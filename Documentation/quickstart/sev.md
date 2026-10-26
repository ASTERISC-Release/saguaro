# Quickstart: AMD SEV Confidential Virtual Machine (`saguaro-sev`)

`saguaro-sev` runs a Saguaro micro-VM as an AMD SEV-SNP guest. SEV-SNP protects guest
memory and registers from the host and supports hardware-signed attestation. The guest kernel is
loaded from an IGVM file measured at launch. See the [VM quickstart](vm.md) and the
dev docs: `sev/PLAN.md`.

## Requirements

- `podman` for the container of `container/run.sh`. The SEV tools are built inside the container;
  nothing is installed on the host.

### Hardware

All the requirements from [VM quickstart](vm.md) apply, and:

- An AMD EPYC host with SEV/SEV-SNP enabled in the firmware and a host kernel with
  SNP support (`cat /sys/module/kvm_amd/parameters/sev_snp` prints `Y`).
  - Refer to [AMD's SEV-SNP GitHub repository](https://github.com/AMDESE/AMDSEV) for details on enabling SEV-SNP in the firmware and kernel.
- `/dev/sev` readable and writable by the user (add the user to the `sev` group on the host).

### Emulation (Optional)

We have also designed a functional emulation of SEV/SEV-SNP (for testing/development). For this, you need:

- Our fork of QEMU--[`qemu-sev-tcg`](https://github.com/adilahmad17/qemu-sev-tcg)--from the `dev-sev` branch,
  pinned as the git submodule `vmm/sev-tcg` (a git clone of this repository is needed; a ZIP download does not
  contain it).
  - Fetch and build it inside the podman container, into `vmm/sev-tcg/prefix`:

        container/run.sh 'scripts/qemu.sh build sev-tcg'

  - The emulation does not use `sev-tools/`: the `build-sev-tools.sh` step of section 1 can be left out.

# 1. Build and install

The container is opened as in [direct.md](direct.md), section 1. Two steps build the SEV tools and
Saguaro. QEMU 11.1.2 with `sev-snp-guest` and IGVM (including a local patch for plain SEV),
`snpguest`, `sevctl` and `sev-snp-measure` are built once into `sev-tools/`, next to the checkout
(it survives the recreation of the container):

    cd <repo-home>
    mkdir -p ../sev-tools      # before the first container/run.sh: it is mounted only if it already exists
    container/run.sh "container/sev/build-sev-tools.sh all"
    container/run.sh "scripts/install.sh --sev --graphics disabled"   # vm PAL + SEV PAL + saguaro-sev

`--sev` builds the normal vm PAL as well, into `~/.local/saguaro-vm` (the same prefix as
`--vm`); `saguaro-sev` uses the QEMU of `sev-tools/` when `QEMU_PATH` is not set.
`--graphics disabled` leaves out the graphics paths, which the SEV guest does not use.
`container/run.sh` loads `~/.local/saguaro-vm/env.sh` by itself, so the commands below need no
environment step.

# 2. Example: Hello World

In the container shell, `exec.sh` runs the example under SEV-SNP (`--sev-snp`) or under plain SEV
(`--sev`):

    cd CI-Examples/helloworld
    ./exec.sh --sev-snp
    ./exec.sh --sev

If you are using **emulation** mode, export the following environment variables first, in the container
shell at the root of the checkout:

    export SAGUARO_ACCEL=tcg QEMU_PATH=$(scripts/qemu.sh path sev-tcg)
    cd CI-Examples/helloworld
    ./exec.sh --sev            # or --sev-snp; every example's exec.sh works

The script builds the example, sets `SAGUARO_SEV_MODE` to `snp` or `sev` and runs `saguaro-sev`.
The output contains:

    saguaro-sev: snp launch digest of .../sev/saguaro-sev.igvm on 1 vCPUs: <96 hex digits>
    Hello, world
    [ VM exited with code 0 ]

The steps by hand:

    cd CI-Examples/helloworld
    make
    SAGUARO_SEV_MODE=snp saguaro-sev helloworld

The digest depends on the PAL build and vCPU count; the attestation report must contain this value.

The guest's serial log (`SAGUARO_VM_SERIAL_LOG=<file>`) starts with `SEV: memory encryption active: SEV SEV-ES SEV-SNP (SEV_STATUS 0x7), C-bit 51`. The SEV PAL refuses to run without
encryption.

`./exec.sh --sev` and `./exec.sh --sev-snp` pass for every example that supports them
([RESULTS.md](../../CI-Examples/RESULTS.md)).


Note the following:

- Set `SAGUARO_CPU_NUM` and `SAGUARO_RAM_SIZE` as for `saguaro-vm`. The guest needs at least 6G
  (the launcher default and the scripts of `test/sev/` use 8G, which SEV pins at launch): `4G`
  no longer works, the guest stops with `Cannot initialize the multiprocess frame pool: Not enough memory`.
- Mode can be selected with `SAGUARO_SEV_MODE` or `sev.mode` in the manifest (`snp` by default, or `sev`).
- `SAGUARO_SEV_POLICY` or `sev.policy` sets the guest policy (default `0x30000` for SNP, with
  debugging disabled).

# 3. Attestation

Inside the guest, `/dev/attestation/attestation_type` reads `sev-snp`; write 64 bytes to
`/dev/attestation/user_report_data` and read `/dev/attestation/quote` (or `report`): the
1184-byte SNP attestation report with those bytes as its report data. `test/sev/sev_attest.c` does
this and writes the report to a host file:

    cd test/sev && make
    SAGUARO_SEV_MODE=snp saguaro-sev sev_attest $(head -c 64 /dev/urandom | od -An -tx1 | tr -d ' \n') /tmp/report.bin

Check the report (in the container, where the SEV tools are): its signature against AMD's certificate chain (fetched from AMD's key
server) and its measurement against the IGVM file:

    export PATH=/root/sev-tools/prefix/bin:$PATH
    snpguest fetch ca pem /tmp/certs milan -e vcek
    snpguest fetch vcek pem /tmp/certs /tmp/report.bin
    python3 ~/.local/saguaro-vm/lib/x86_64-linux-gnu/saguaro/sev/saguaro-sev-igvm.py verify-snp \
        ~/.local/saguaro-vm/lib/x86_64-linux-gnu/saguaro/sev/saguaro-sev.igvm /tmp/report.bin \
        --vcpus 1 --certs /tmp/certs

which ends with `ok   signature by the VCEK` and `OK`. The measurement covers the PAL and its
boot state; the library OS, the application and its files come from the host through virtiofs and
are currently not measured.

# 4. Regression Tests

    test/quick.sh sev-snp                         # the regression tier under SNP, at 2 vCPUs and 8G
    test/quick.sh sev                             # the same under plain SEV (launch measurement instead of attestation)
    test/quick.sh sev-snp --tier smoke            # a few minutes
    test/quick.sh sev-snp --tier extended         # the full LibOS suite at 2, 1 and 4 vCPUs, loops, stress tests: hours
    SAGUARO_ACCEL=tcg test/quick.sh sev-snp --tier smoke    # under the emulation (QEMU_PATH as above)

The regression tier runs one vCPU count, 2: the LibOS core list (127 tests, the same as on every variant), six
examples through `exec.sh --sev-snp`, `test/sev/test-sev.sh --vcpus 2` and the multi-process probes; it prints a
summary line to keep with a release (there is no CI for SEV). The scripts underneath:

    SAGUARO_SEV_MODE=snp test/sev/test-sev.sh     # hello (1, 4 vCPUs), host-side memory check, vsock, attest
    test/sev/test-sev.sh --vcpus 2                # the same suites, once, at 2 vCPUs
    test/sev/test-sev-all.sh                      # the qualification run: builds, both modes, the LibOS suite at 2 vCPUs, examples

`SAGUARO_SEV_MODE=snp test/sev/test-sev.sh` runs under SNP, `test/sev/test-sev.sh` under plain SEV
(see the header of the script for the suites). `test/sev/test-sev.sh` passes in both modes
([RESULTS.md](../../CI-Examples/RESULTS.md)).

# 5. Troubleshooting

- `exec.sh` reports that `sev` cannot run: it names the reason. No `/dev/sev`: add the user to the
  `sev` group on the host, log in again; the container may have to be recreated to see the device (`container/run.sh down`
  discards the install inside it, then repeat section 1; not verified).
- `sev_snp` in `/sys/module/kvm_amd/parameters` is not `Y`: SNP is off in the firmware or the host
  kernel; plain SEV (`--sev`) may still work.
- `saguaro-sev` cannot find QEMU: `container/sev/build-sev-tools.sh all` has not run, or
  `sev-tools/` is not next to the checkout. Set `SEV_TOOLS_DIR` or `QEMU_PATH`.
- `Cannot initialize the multiprocess frame pool: Not enough memory`: `SAGUARO_RAM_SIZE` is below
  6G; unset it or use `8G`.
- The signature check of an attestation report needs network access to AMD's key server.
