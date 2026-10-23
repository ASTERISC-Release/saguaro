<p align="center">
  <img src="Documentation/img/saguaro-icon.svg" alt="Saguaro icon: a saguaro cactus against a desert sunset" width="144">
</p>

# Saguaro

*A library OS for sandboxed and confidential workloads, derived from Gramine.*

Saguaro is a Linux-compatible library OS (LibOS) for multi-process applications, on Intel x86_64, AMD64, AArch64 and RISC-V. It runs an unmodified Linux application in an isolated environment, with its own implementation of the Linux (POSIX) system call interface (the *LibOS*) on top of a small host abstraction layer (the *PAL*).

Saguaro is a derivative of [Gramine](https://github.com/gramineproject/gramine) (formerly *Graphene*), an 
excellent Library OS designed and built by a diverse group of researchers and engineers from universities and industry 
to run on Intel x86 platforms. Our work is not affiliated with or endorsed by the Gramine project or LF Projects. Please 
see the [Acknowledgements](#acknowledgement) section. 

Keeping [upstream](https://github.com/gramineproject/gramine-tdx)'s full history, Saguaro extends Gramine to provide wide support 
for additional backends, namely

- Containerized userspace (`direct`)
- Isolated userspace (`sgx`)
- Micro-virtual machine (`vm`)
- Windows protected virtual machine (`vbs`)
- Confidential virtual machine (`tdx`, `sev`)

Additionally, several quality-of-life [features](Documentation/FEATURES.md) have been added, including

- **Multi-architecture support:** AMD64, AArch64 and RISC-V.
- **Graphics acceleration:** redirection to the host GPU in the `vm`/`direct` PAL (see [Tirith](https://arxiv.org/abs/2609.17525)).
  - Experimentally, it can even play (some) Windows games using Wine/Proton in the `vm` PAL.
- **Richer system call coverage:** including multi-process (e.g., fork, clone), signaling, and network.

These features and backends are the result of published (and upcoming) work by the ASU Trusted, Reliable, and Secure Computing
([ASTERISC](https://asteriscresearchlab.com/)) Research Lab. Refer to this [section](#citation) for details.


> **Disclaimer:** The current work is **experimental** and should not be used in production environments. Although all design decisions were made by our team, 
an AI agent (namely our lab's *riscy* agent) was heavily-involved in the development of this project as evident from the commit history.
Our rationale for using AI agents was to easily prototype and merge together disjoint artifacts from different research projects and ideas, in order 
to make them available for research and teaching.

## Use-Cases

There are three main use-cases of this project:

**(1) Sandboxing untrusted Linux applications (e.g., AI agents and games):** An application runs
on Saguaro's own system call interface rather than directly on the host kernel, reducing the
host interface that the application can reach. The `direct` PAL runs it as a lightweight
container-like environment. The `vm` PAL runs it in a minimal virtual machine that holds only
the application, the library OS and a small kernel. Graphics-intensive applications are supported through
redirection to the host GPU in an efficient manner.

**(2) Running unmodified Linux applications in diverse protected contexts:** The same library OS
runs an application inside hardware-protected environments, so that it is protected from the
platform that hosts it: Intel SGX enclaves (`sgx`), Intel TDX and AMD SEV confidential virtual
machines (`tdx`, `sev`) and Windows VBS enclaves (`vbs`). The application does not need to be modified
or ported to a different enclave interface. Remote attestation (implemented by the hardware/OS vendor) lets a 
remote party verify what is running.

**(3) Teaching Linux-inspired operating system and protected computing implementations:** The
library OS and the bare-metal kernel of the `vm` PAL are compact and readable. The same Linux-compatible 
interface is implemented on x86_64, AArch64 and RISC-V, so that the architecture-specific parts can be compared 
directly. The same applies to protected contexts like TDX/SEV/VBS, where the underlying PAL shows how the same
feature is implemented in different hardware/software contexts.

## Platform Support

| Symbol | Brief Description |
|:---:|:---:|
| ✓ | supported: works and is covered by tests |
| ◐ | experimental: runs, but incomplete, with known limits, or not yet fully validated |
| ○ | planned: on the roadmap, not started or not usable yet |
| ✗ | not supported: possible in principle, but it does not work (yet) |
| – | not applicable: the combination does not exist |

### Sandboxed Environments

| Platform Adaptation Layer (PAL)   | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|:---:|:---:|:---:|:---:|:---:|
| `direct` (Container)              | ✓           | ✓    | ✓      | ◐            |
| `direct` with redirected graphics | ◐           | ◐    | ◐      | ✗            |
| `vm` (MicroVM)               | ✓           | ◐    | ◐      | ○            |
| `vm` with redirected graphics     | ◐           | ◐    | ○      | ✗            |

### Protected Environments

| Platform Adaptation Layer (PAL) | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|:---:|:---:|:---:|:---:|:---:|
| `sgx` (Intel SGX)               | ◐           | –    | –      | –            |
| `tdx` (Intel TDX)               | ◐           | –    | –      | –            |
| `sev` (AMD SEV)                 | –           | ◐    | –      | –            |
| `vbs` (Windows VBS enclaves)    | ◐           | ◐    | ◐      | –            |

Please see [CHANGELOG.md](CHANGELOG.md) and [TODO.md](Documentation/TODO.md) for a summary.

### Continuous Integration

The workflows run on hosted GitHub runners and are started by hand (`workflow_dispatch`); each badge shows the last
run on `main`. Hardware-bound backends (`sgx`, `tdx`, `sev`, `vbs`) are tested manually on the hardware.

| Workflow | Intel x86_64 | AArch64 | RISC-V (RV64, emulated) |
|:---:|:---:|:---:|:---:|
| `direct` (Container) | [![direct x86_64](https://github.com/ASTERISC-Release/saguaro/actions/workflows/direct-x86_64.yml/badge.svg?branch=main&event=workflow_dispatch)](https://github.com/ASTERISC-Release/saguaro/actions/workflows/direct-x86_64.yml) | [![direct aarch64](https://github.com/ASTERISC-Release/saguaro/actions/workflows/direct-aarch64.yml/badge.svg?branch=main&event=workflow_dispatch)](https://github.com/ASTERISC-Release/saguaro/actions/workflows/direct-aarch64.yml) | [![direct riscv64](https://github.com/ASTERISC-Release/saguaro/actions/workflows/direct-riscv64-emulated.yml/badge.svg?branch=main&event=workflow_dispatch)](https://github.com/ASTERISC-Release/saguaro/actions/workflows/direct-riscv64-emulated.yml) |
| `vm` (MicroVM)       | [![vm x86_64](https://github.com/ASTERISC-Release/saguaro/actions/workflows/vm-x86_64.yml/badge.svg?branch=main&event=workflow_dispatch)](https://github.com/ASTERISC-Release/saguaro/actions/workflows/vm-x86_64.yml) | [![vm aarch64 (TCG)](https://github.com/ASTERISC-Release/saguaro/actions/workflows/vm-aarch64-tcg.yml/badge.svg?branch=main&event=workflow_dispatch)](https://github.com/ASTERISC-Release/saguaro/actions/workflows/vm-aarch64-tcg.yml) | – |

[![documentation check](https://github.com/ASTERISC-Release/saguaro/actions/workflows/docs-check.yml/badge.svg?branch=main&event=workflow_dispatch)](https://github.com/ASTERISC-Release/saguaro/actions/workflows/docs-check.yml)

## Getting Started

Get the source:

    git clone https://github.com/ASTERISC-Release/saguaro

Each quickstart guide goes from a checkout to a running example:

- **Containerized userspace** (`direct`): [Documentation/quickstart/direct.md](Documentation/quickstart/direct.md)
- **Micro-virtual machine** (`vm`): [Documentation/quickstart/vm.md](Documentation/quickstart/vm.md)
- **Windows VBS enclaves** (`vbs`): [Documentation/quickstart/vbs-windows.md](Documentation/quickstart/vbs-windows.md)
- **Intel TDX and SGX** (`tdx`, `sgx`): [Documentation/quickstart/tdx-sgx.md](Documentation/quickstart/tdx-sgx.md)
- **AMD SEV-SNP** (`sev`): [Documentation/quickstart/sev.md](Documentation/quickstart/sev.md)

Other important guides include: 

- Support for running graphical applications under [userspace](https://github.com/ASTERISC-Release/saguaro/blob/main/Documentation/quickstart/graphics-direct.md) 
and [microVM](https://github.com/ASTERISC-Release/saguaro/blob/main/Documentation/quickstart/graphics-vm.md) 
- Cross-building RISC-V [userspace](https://github.com/ASTERISC-Release/saguaro/blob/main/Documentation/quickstart/riscv.md) under an emulated guest environment 
- The complete reference for [building and installing](INSTALL.md), on a host or in the supplied containers

## Contributing

Bug reports and pull requests are welcome through the GitHub repository
([ASTERISC-Release/saguaro](https://github.com/ASTERISC-Release/saguaro)); see
[CONTRIBUTING.md](CONTRIBUTING.md), which also asks you to mark AI-generated content. By
submitting a contribution you agree that it is licensed under the license of the
file it modifies or adds (LGPL-3.0-or-later, or BSD-3-Clause for files under
`CI-Examples/`), and that you have the right to submit it under that license.

## License

Saguaro, like Gramine, is licensed under the LGPL-3.0-or-later (see `LICENSE.txt`); the
examples in `CI-Examples/` are BSD-3-Clause unless a file says otherwise (see
`LICENSE.addendum.txt`). The copyright notices of upstream Gramine and Gramine-TDX
contributors are retained in their original format.

## Citation

We are in the process of releasing a whitepaper to Arxiv detailing the current project. For now, we refer 
you to the following published work by our lab (out of which this work is born):

- You Shall Not Pass into Ring-0! A User Privacy-Friendly Anti-Cheat Architecture for Personal Computers (ACM CCS'26)
- Reducio: Optimized Confidential Serverless Cloud Deployments for Enterprise Customers (Arxiv)
- Erebor: A Drop-In Sandbox Solution for Private Data Processing in Untrusted Confidential Virtual Machines (ACM EuroSys'25)
- Veil: A Protected Services Framework for Confidential Virtual Machines (ACM ASPLOS'23)
- Chancel: Efficient Multi-client Isolation Under Adversarial Programs (ISOC NDSS'21)

If you find this project useful, we request that you cite our work.

## Acknowledgement

Saguaro is derived from Gramine's Intel TDX and virtual-machine fork, namely [gramine-tdx](https://github.com/gramineproject/gramine-tdx). We thank the Gramine/Gramine-TDX contributors; the organizations and individuals are listed in [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md).
