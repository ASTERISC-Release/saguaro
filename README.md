# Gramine Library OS with Multi-Architectural Support

*A Linux-compatible library OS (LibOS) for multi-process applications, on Intel x86_64, AMD64,
AArch64 and RISC-V.*

Gramine (formerly *Graphene*) runs an unmodified Linux application in an isolated environment, with its own
implementation of the Linux (POSIX) system call interface (the *LibOS*) on top of a small host abstraction
layer (the *PAL*). It was initially designed and built by a diverse group of researchers
and engineers from universities and industry to run on Intel x86 platforms. Please see the
[Acknowledgements](#acknowledgements) section.

Keeping [upstream](https://github.com/gramineproject/gramine-tdx)'s full history, this Gramine fork has been extended to provide wide-support for additional backends, namely

- Containerized userspace (`direct`)
- Isolated userspace (`sgx`)
- Micro-virtual machine (`vm`)
- Windows protected virtual machine (`vbs`)
- Confidential virtual machine (`tdx`, `sev`)

Additionally, several quality-of-life [features](Documentation/FEATURES.md) have been added, including

- **Multi-architecture support:** AMD64, AArch64 and RISC-V.
- **Graphics redirection:** redirection to the host GPU in the `vm`/`direct` PAL (see [Tirith](https://arxiv.org/abs/2609.17525)).
  - Even allows us to play Windows games using Wine and Proton in the `vm` PAL.
- **Richer system call coverage:** including multi-process (e.g., fork, clone), signaling, and network.

These features and backends are the result of published and upcoming work by the [ASU Trusted, Reliable, and Secure Computing
(ASTERISC) Research Lab](https://asteriscresearchlab.com/). Refer to this [section](#citation) for details and how to cite our work.


> **Disclaimer:** The current work is **experimental** and should not be used in production environments. Although all design decisions were made by our team, 
an AI agent (namely our lab's *riscy* agent) was heavily-involved in the development of this project (as evident from the commit history).
Our rationale for using AI agents was to easily prototype and merge together disjoint artifacts from different research projects
and make them available for research and teaching.

## Use-Cases

There are three main use-cases of this project:

**(1) Sandboxing untrusted Linux applications (e.g., AI agents and games):** An application runs
on Gramine's own system call interface rather than directly on the host kernel, reducing the
host interface that the application can reach. The `direct` PAL runs it as a lightweight
container-like environment. The `vm` PAL runs it in a minimal virtual machine that holds only
the application, the library OS and a small kernel. Graphics-intensive applications are supported through
redirection to the host GPU in an efficient manner.

**(2) Running unmodified Linux applications in diverse protected contexts:** The same library OS
runs an application inside hardware-protected environments, so that it is protected from the
platform that hosts it: Intel SGX enclaves (`sgx`), Intel TDX and AMD SEV confidential virtual
machines (`tdx`, `sev`) and Windows VBS enclaves (`vbs`). The application does not need to be modified
or ported to a different enclave interface. Remote attestation lets a remote party verify what is running.

**(3) Teaching Linux-inspired operating system implementations on different architectures:** The
library OS and the small bare-metal kernel of the `vm` PAL (page tables, scheduler, interrupts,
and virtio drivers) are compact and readable. The same Linux-compatible interface is implemented
on x86_64, AArch64 and RISC-V, so that the architecture-specific parts can be compared directly.

## Platform Support

|  |  |
|----|----|
| ✅ | supported: works and is covered by tests |
| ⚠️ | experimental: runs, but incomplete, with known limits, or not yet fully validated |
| 🛫 | planned: on the roadmap, not started or not usable yet |
| ❌ | not supported: possible in principle, but it does not work (yet) |
| ➖ | not applicable: the combination does not exist |

### Sandboxed Environments

| Platform Adaptation Layer (PAL)   | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|-----------------------------------|--------------|-------|---------|---------------|
| `direct`                          | ✅           | ✅    | ✅      | ⚠️            |
| `direct` with redirected graphics | ⚠️           | ⚠️    | ⚠️      | ❌            |
| `vm` (`gramine-vm`)               | ✅           | ⚠️    | ⚠️      | 🛫            |
| `vm` with redirected graphics     | ⚠️           | ⚠️    | 🛫      | ❌            |

### Protected Environments

| Platform Adaptation Layer (PAL) | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|---------------------------------|--------------|-------|---------|---------------|
| `sgx` (Intel SGX)               | ⚠️           | ➖    | ➖      | ➖            |
| `tdx` (Intel TDX)               | ⚠️           | ➖    | ➖      | ➖            |
| `sev` (AMD SEV)                 | ➖           | ⚠️    | ➖      | ➖            |
| `vbs` (Windows VBS enclaves)    | ⚠️           | ⚠️    | ⚠️      | ➖            |

Per-cell details, build options and verification status are in the development notes (a private repository; see [CHANGELOG.md](CHANGELOG.md) and [TODO.md](Documentation/TODO.md) for the public summary).

## Getting Started

Each quickstart guide goes from a checkout to a running example:

- **Containerized userspace** (`direct`): [Documentation/quickstart/direct.md](Documentation/quickstart/direct.md)
- **Micro-virtual machine** (`vm`): [Documentation/quickstart/vm.md](Documentation/quickstart/vm.md)
- **Windows VBS enclaves** (`vbs`): [Documentation/quickstart/vbs-windows.md](Documentation/quickstart/vbs-windows.md)
- **Intel TDX and SGX** (`tdx`, `sgx`): [Documentation/quickstart/tdx-sgx.md](Documentation/quickstart/tdx-sgx.md)
- **AMD SEV-SNP** (`sev`): [Documentation/quickstart/sev.md](Documentation/quickstart/sev.md)

Guides for graphics and for the RISC-V cross-build are listed in the [quickstart index](Documentation/quickstart/README.md).

[INSTALL.md](INSTALL.md) is the complete reference
for building and installing, on a host or in the supplied containers.
[Documentation/README.md](Documentation/README.md) lists the documents of this tree, and
[AGENTS.md](AGENTS.md) says how to build and run each mode (for people and coding agents).

`CI-Examples/` contains ready-made applications.

## Contributing

Bug reports and pull requests are welcome through the GitHub repository. By
submitting a contribution you agree that it is licensed under the license of the
file it modifies or adds (LGPL-3.0-or-later, or BSD-3-Clause for files under
`CI-Examples/`), and that you have the right to submit it under that license.

## License

Gramine is licensed under the LGPL-3.0-or-later (see `LICENSE.txt`); the
examples in `CI-Examples/` are BSD-3-Clause unless a file says otherwise (see
`LICENSE.addendum.txt`). The code added by the ASTERISC Research Lab (Arizona State
University) is copyright (C) 2026 The ASTERISC Research Lab and is licensed under the
same terms as the part of the tree it belongs to; the addendum lists the third-party
material this tree added. The copyright notices of upstream Gramine and gramine-tdx
are retained.

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

This work builds on [Gramine](https://github.com/gramineproject/gramine) and its
Intel TDX and virtual-machine fork [gramine-tdx](https://github.com/gramineproject/gramine-tdx). We thank the Gramine and gramine-tdx authors and contributors; the organizations and individuals are listed in
[ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md).
