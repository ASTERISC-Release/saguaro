# Introduction to SGX

> [!NOTE]
> This page is inherited from upstream Gramine and describes the Intel SGX backend. This tree
> still builds that backend but does not test it; see `README.md` for what is validated here.

The Gramine project uses the `Intel SGX <SGX>` (Software Guard Extensions)
technology to protect software running on untrusted hosts. SGX is a \|~\|
complicated topic, which may be hard to learn, because the documentation is
scattered through official/reference documentation, blogposts and academic
papers. This page is an attempt to curate a \|~\| dossier of available reading
material.

SGX is an umbrella name of *technology* that comprises several parts:

- CPU/platform *hardware features*: the instruction set, microarchitecture with
  the `PRM` memory region and some new MSRs, some new logic in the MMU
  and so on;
- the SGX `Remote Attestation` *infrastructure*, online services provided
  by Intel and/or third parties (see `DCAP`);
- `SDK` and assorted *software*.

SGX is still being developed. The current (March 2024) version of CPU features
is referred to as "`SGX2`" or simply "SGX". The older instruction set
from the original SGX is informally referred to as "`SGX1`".

Features which might be considered part of SGX2:

- `EDMM` (Enclave Dynamic Memory Management)
- `FLC` (Flexible Launch Control; not strictly part of SGX2, but was not
  part of original SGX hardware either)
- `KSS` (Key Separation and Sharing; also not part of SGX2, but was not
  part of original SGX hardware either)

Around 2022 Intel discontinued SGX support in client CPU cores, and instead
introduced it to server cores. The new SGX hardware architecture didn't change
the user-facing ABI, but loosened security guarantees, matching AMD SEV-SNP
security model:

- Merkle tree for memory integrity checking was removed.
- Hardware RAM MitM attacks are not mitigated anymore: (because of Merkle tree
  removal)
  - On Icelake server CPUs there's no integrity protection at all.
  - On Sapphire Rapids server CPUs there's a 28-bit MAC per each cacheline.
    It's possible to bruteforce the MAC or do a replay attack with cacheline
    granularity (but that still requires a hardware MitM).
- EPC can now be almost arbitrarily big, significantly improving performance for
  large workloads.

As of now most of the broadly used server CPUs support `SGX2`. Only older
client CPUs support SGX, so they should not be used in production (because of
missing security patches for side-channels).

## Introductory reading

> [!NOTE]
> Most of the older literature available (especially introduction-level)
> concerns the original `SGX1` only.

- Quarkslab's two-part "Overview of Intel SGX":
  - [Part 1, SGX Internals (Quarkslab)](https://blog.quarkslab.com/overview-of-intel-sgx-part-1-sgx-internals.html)
  - [Part 2, SGX Externals (Quarkslab)](https://blog.quarkslab.com/overview-of-intel-sgx-part-2-sgx-externals.html)
- [MIT's deep dive in SGX architecture](https://eprint.iacr.org/2016/086).
- Intel's whitepapers:
  - [Innovative Technology for CPU Based Attestation and Sealing](https://software.intel.com/en-us/articles/innovative-technology-for-cpu-based-attestation-and-sealing)
  - [Innovative Instructions and Software Model for Isolated Execution](https://software.intel.com/en-us/articles/innovative-instructions-and-software-model-for-isolated-execution)
  - [Using Innovative Instructions to Create Trustworthy Software Solutions \[PDF\]](https://software.intel.com/sites/default/files/article/413938/hasp-2013-innovative-instructions-for-trusted-solutions.pdf)
  - [Slides from ISCA 2015](https://sgxisca.weebly.com/)
    ([actual slides \[PDF\]](https://software.intel.com/sites/default/files/332680-002.pdf))
- [Hardware compatibility list (unofficial)](https://github.com/ayeks/SGX-hardware)

## Official documentation

- [Intel® 64 and IA-32 Architectures Software Developer's Manual Volume 3D:
  System Programming Guide, Part 4](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3d-part-4-manual.pdf)
- [SDK for Linux](https://01.org/intel-software-guard-extensions/downloads)
  (download of both the binaries and the documentation)

## Academic research

- [Intel's collection of academic papers](https://software.intel.com/en-us/sgx/documentation/academic-research),
  likely the most comprehensive list of references

## Installation instructions

See `sgx-setup`.

### Linux kernel drivers

For historical reasons, there are three SGX drivers currently (March 2024):

- <https://github.com/intel/linux-sgx-driver> -- old one, does not support DCAP,
  deprecated

- <https://github.com/intel/SGXDataCenterAttestationPrimitives/tree/master/driver>
  -- out-of-tree, supports both non-DCAP software infrastructure (with old EPID
  remote-attestation technique) and the new DCAP (with new ECDSA and
  more "normal" PKI infrastructure). Deprecated in favor of the upstreamed
  driver (see below).

- The upstreamed Linux driver -- SGX support was upstreamed to the Linux
  mainline starting from 5.11. It currently supports only DCAP attestation.

  Also, it doesn't require `IAS` and kernel maintainers consider
  non-writable `FLC` MSRs as non-functional SGX:
  <https://lore.kernel.org/lkml/20191223094614.GB16710@zn.tnic/>

## SGX terminology

<div class="glossary">

Architectural Enclaves
AE

> Architectural Enclaves (AEs) are a \|~\| set of "system" enclaves concerned
> with starting and attesting other enclaves. Intel provides reference
> implementations of these enclaves, though other companies may write their
> own implementations.
>
> <div class="seealso">
>
> `Provisioning Enclave`
>
> `Launch Enclave`
>
> `Quoting Enclave`
>
> </div>

Architectural Enclave Service Manager
AESM

> The Architectural Enclave Service Manager is responsible for providing SGX
> applications with access to the `Architectural Enclaves`. It consists
> of the Architectural Enclave Service Manager Daemon, which hosts the enclaves,
> and a component of the SGX SDK, which communicates with the daemon over a Unix
> socket with the fixed path `/var/run/aesmd/aesm.sock`.

Asynchronous Enclave Exit
AEX

> An event caused by an exception occurring during in-enclave execution. CPU
> saves the current context into `SSA`, leaves SGX mode and jumps
> to `AEP`.

Asynchronous Exit Pointer
AEP

> An address outside the enclave where CPU will jump in case an exception
> happens during in-enclave execution.

Attestation

> Attestation is a mechanism to prove the trustworthiness of the SGX enclave
> to a local or remote party. More specifically, SGX attestation proves that
> the enclave runs on a real hardware in an up-to-date TEE with the expected
> initial state. There are two types of the attestation:
> `Local Attestation` (between enclaves on the same machines)
> and `Remote Attestation` (between enclave and any party, possibly
> remote).
>
> <div class="seealso">
>
> `attestation`
>
> `Local Attestation`
>
> `Remote Attestation`
>
> </div>

Data Center Attestation Primitives
DCAP

> A \|~\| software infrastructure provided by Intel as a reference
> implementation for the new ECDSA/`PCS`-based remote attestation.
> Relies on the `Flexible Launch Control` hardware feature.
>
> This allows for launching enclaves with Intel's remote infrastructure
> only involved in the initial setup. Naturally however, this requires
> deployment of own infrastructure, so is operationally more complicated.
> Therefore it is intended for server environments (where you control all
> the machines).
>
> <div class="seealso">
>
> Orientation Guide  
> <https://download.01.org/intel-sgx/latest/dcap-latest/linux/docs/DCAP_ECDSA_Orientation.pdf>
>
> `EPID`  
> A \|~\| way to launch enclaves with Intel's infrastructure, intended
> for client machines.
>
> </div>

ECALL

> A \|~\| special function call made by non-enclave world into an enclave.

Enclave

> An instance of SGX TEE, residing in a contiguous chunk of usermode address
> space (`ELRANGE`) of some process on the system. Application threads
> may enter and exit the enclave through dedicated CPU instructions. Code
> running inside an enclave has access to usermode memory of the process
> which contains it, but not the other way.

Enclave Dynamic Memory Management
EDMM

> A \|~\| hardware feature of `SGX2`, allows for dynamic (in enclave
> runtime) addition and removal of enclave threads and memory, as well as
> changing memory permissions and type.

Enclave Page Cache
EPC

> A \|~\| part of `PRM` used for caching enclave pages. `EPC` is
> only an optimization and its size doesn't limit possible enclave sizes,
> though too-small `EPC` may lead to frequent page swapping and
> significantly worsen performance.

Enclave Page Cache Map
EPCM

> A \|~\| part of `PRM` which holds metadata about EPC pages.

Enhanced Privacy Identification
Enhanced Privacy Identifier
EPID

> EPID is the attestation protocol originally shipped with SGX. Unlike
> `DCAP`, a \|~\| remote verifier making use of the EPID protocol needs
> to contact the `Intel Attestation Service` each time it wishes
> to attest an \|~\| enclave.
>
> Contrary to DCAP, EPID may be understood as "opinionated", with most
> moving parts fixed and tied to services provided by Intel. This is
> intended for client enclaves and deprecated for server environments.
>
> EPID attestation can operate in two modes: *fully-anonymous (unlinkable)
> quotes* and *pseudonymous (linkable) quotes*. Unlike fully-anonymous
> quotes, pseudonymous quotes include an \|~\| identifier dependent on the
> identity of the CPU and the developer of the enclave being quoted, which
> allows determining whether two instances of your enclave are running on
> the same CPU or not.
>
> If your security model depends on enforcing that the identifiers are
> different (e.g. because you want to prevent sybil attacks), keep in mind
> that the enclave host can generate a new identity by performing an
> epoch reset. The previous identity will then become inaccessible, though.
>
> The attestation mode being used can be chosen by the application enclave,
> but it must match what was chosen when generating the `SPID`.
>
> <div class="seealso">
>
> `DCAP`  
> A way to launch enclaves without relying on the Intel's
> infrastructure (after initial setup).
>
> `SPID`  
> An identifier one can obtain from Intel, required to make use of EPID
> attestation.
>
> </div>

Flexible Launch Control
FLC

> Hardware (CPU) feature that allows substituting `Launch Enclave` for
> one not signed by Intel through a \|~\| change in SGX's EINIT logic to not
> require the EINITTOKEN from the Intel-based Launch Enclave. An \|~\| MSR,
> which can be locked at boot time, keeps the hash of the public key of
> the "launching" entity.
>
> With FLC, `Launch Enclave` can be written by other companies (other
> than Intel) and must be signed with the key corresponding to the one
> locked in the MSR (a \|~\| reference Launch Enclave simply allows all
> enclaves to run). The MSR can also stay unlocked and then it can be
> modified at run-time by the VMM or the OS kernel.
>
> Support for FLC can be detected using `CPUID` instruction, as
> `CPUID.07H:ECX.SGX_LC[bit 30] == 1` (SDM vol. 2A calls this "SGX Launch
> Control").
>
> <div class="seealso">
>
> <https://software.intel.com/en-us/blogs/2018/12/09/an-update-on-3rd-party-attestation>  
> Announcement
>
> `DCAP`
>
> </div>

Key Separation and Sharing
KSS
A feature that lets developer define additional enclave identity
attributes and configuration identifier. Extended enclave identity
is defined by the developer on enclave build. Enclave configuration is
defined on enclave launch and cannot be modified afterwards.

> In addition to the calculated enclave and signer measurements, developer
> is expected to define a product ID and `SVN` for her enclaves.
> These identifiers are part of the `SGX Report` and are expected to
> be used in `Attestation`. They are also used by SGX key derivation
> to derive different keys per configuration.
>
> KSS adds two more attributes for enclave build and two new ones for
> enclave launch, which are part of the `SGX Report`.
> Additionally, key policy attributes are extended to provide fine-grained
> control over key derivation.
>
> New build attributes:
>
> - Extended product ID
> - Family ID
>
> New enclave launch attributes:
>
> - Config ID
> - Config SVN
>
> This feature was not part of original SGX and therefore is not supported
> by all SGX-enabled hardware.

Launch Enclave
LE

> <div class="todo">
>
> TBD
>
> </div>
>
> <div class="seealso">
>
> `Architectural Enclaves`
>
> </div>

Local Attestation

> In local attestation, the attesting SGX enclave collects attestation
> evidence in the form of an `SGX Report` using the EREPORT hardware
> instruction. This form of attestation is used to send the attestation
> evidence to a local party (on the same physical machine).
>
> <div class="seealso">
>
> `attestation`
>
> </div>

Intel Attestation Service
IAS

> Internet service provided by Intel for "old" `EPID`-based remote
> attestation. Enclaves send SGX quotes to the client/verifier who will
> forward them to IAS to check their validity.
>
> <div class="seealso">
>
> `PCS`  
> Provisioning Certification Service, another Internet service
> provided by Intel.
>
> </div>

Memory Encryption Engine
MEE

> <div class="todo">
>
> TBD
>
> </div>

OCALL

> A \|~\| special function call made by an enclave to the non-enclave world.

SGX Platform Software
PSW

> Software infrastructure provided by Intel with all special
> `Architectural Enclaves` (`Provisioning Enclave`,
> `Quoting Enclave`, `Launch Enclave`). This mainly refers to
> the "old" EPID/IAS-based remote attestation.

Processor Reserved Memory
PRM

> A \|~\| mostly undocumented region of physical address space reserved by the
> BIOS for internal use by SGX hardware. Known to contain at
> least `EPC` and `EPCM`.

Provisioning Enclave
PE

> One of the Architectural Enclaves of the Intel SGX software
> infrastructure. It is part of the `SGX Platform Software`. The
> Provisioning Enclave is used in `EPID` based remote attestation.
> This enclave communicates with the Intel Provisioning Service
> (`IPS`) to perform EPID provisioning. The result of this
> provisioning procedure is the private EPID key securely accessed by the
> Provisioning Enclave. This procedure happens only during the first
> deployment of the SGX machine (or, in rare cases, to provision a new EPID
> key after TCB upgrade). The main user of the Provisioning Enclave is the
> `Quoting Enclave`.
>
> <div class="seealso">
>
> `Architectural Enclaves`
>
> </div>

Provisioning Certification Enclave
PCE

> One of the Architectural Enclaves of the Intel SGX software
> infrastructure. It is part of the `SGX Platform Software` and
> `DCAP`. The Provisioning Certification Enclave is used in
> `DCAP` based remote attestation. This enclave communicates with the
> Intel Provisioning Certification Service (`PCS`) to perform DCAP
> provisioning. The result of this provisioning procedure is the DCAP/ECDSA
> attestation collateral (mainly the X.509 certificate chains rooted in a
> well-known Intel certificate and Certificate Revocation Lists). This
> procedure happens during the first deployment of the SGX machine and then
> periodically to refresh the cached attestation collateral. Typically, to
> reduce the dependency on PCS, a cloud service provider introduces an
> intermediate caching service (Provisioning Certification Caching Service,
> or PCCS) that stores all the attestation collateral obtained from Intel.
> The main user of the Provisioning Certification Enclave is the
> `Quoting Enclave`.
>
> <div class="seealso">
>
> `Architectural Enclaves`
>
> </div>

Intel Provisioning Service
IPS

> Internet service provided by Intel for EPID-based remote attestation.
> This service provides the corresponding EPID key to the Provisioning
> Enclave on a remote SGX machine.

Intel Provisioning Certification Service
PCS

> New internet service provided by Intel for new ECDSA-based remote
> attestation. Enclave provider creates its own internal Attestation Service
> where it caches PKI collateral from Intel's PCS, and the verifier gets the
> certificate chain from the enclave provider to check validity.
>
> <div class="seealso">
>
> `IAS`  
> Intel Attestation Service, another Internet service.
>
> </div>

Quoting Enclave
QE

> One of the Architectural Enclaves of the Intel SGX software
> infrastructure. It is part of the `SGX Platform Software`. The
> Quoting Enclave receives an `SGX Report` and produces a
> corresponding `SGX Quote`. The identity of the Quoting Enclave is
> publicly known (it signer, its measurement and its attributes) and is
> vetted by public companies such as Intel (in the form of the certificate
> chain ending in a publicly known root certificate of the company).
>
> <div class="seealso">
>
> `Architectural Enclaves`
>
> </div>

Remote Attestation

> For remote attestation, the attesting SGX enclave collects attestation
> evidence in the form of an `SGX Quote` using the `Quoting
> Enclave` (and the `Provisioning Enclave` if required). The enclave
> then may send the collected attestation evidence to the local or remote
> party, which will verify the evidence and confirm the authenticity and
> integrity of the attested enclave. After this, the local or remote party
> trusts the enclave and may establish a secure channel with the enclave
> and send secrets to it.
>
> <div class="seealso">
>
> `attestation`
>
> </div>

Intel SGX Software Development Kit
Intel SGX SDK
SGX SDK
SDK

> In the context of `SGX`, this means a \|~\| specific piece of software
> supplied by Intel which helps people write enclaves packed into `.so`
> files to be accessible like normal libraries (at least on Linux).
> Available together with a \|~\| kernel module and documentation.

SGX Enclave Control Structure
SECS

> <div class="todo">
>
> TBD
>
> </div>

SGX Quote

> The SGX quote is the proof of trustworthiness of the enclave and is used
> during `Remote Attestation`. The attesting enclave generates the
> enclave-specific `SGX Report`, sends the request to the
> `Quoting Enclave` using `Local Attestation`, and the Quoting
> Enclave returns back the SGX quote with the SGX report embedded in it. The
> resulting SGX quote contains the enclave's measurement, attributes and
> other security-relevant fields, and is tied to the identity of the
> `Quoting Enclave` to prove its authenticity. The obtained SGX quote
> may be later sent to the verifying remote party, which examines the SGX
> quote and gains trust in the remote enclave.

SGX Report

> The SGX report is a data structure that contains the enclave's measurement,
> signer identity, attributes and a user-defined 64B string. The SGX report
> is generated using the `EREPORT` hardware instruction. It is used during
> `Local Attestation`. The SGX report is embedded into the
> `SGX Quote`.

SGX1

> The original SGX instruction set, without dynamic resource management.

SGX2

> New SGX instructions and other hardware features that were introduced
> after the release of the original `SGX1` (e.g. `EDMM`).

Service Provider ID
SPID

> An identifier provided by Intel, used together with an \|~\| `EPID`
> API key to authenticate to the `Intel Attestation Service`. You can
> obtain an \|~\| SPID through Intel's [Trusted Services Portal](https://api.portal.trustedservices.intel.com/EPID-attestation).
>
> See `EPID` for a \|~\| description of the difference between
> *linkable* and *unlinkable* quotes.

State Save Area
SSA

> <div class="todo">
>
> TBD
>
> </div>

Security Version Number
SVN

> Each element of the SGX `TCB` is assigned a Security Version Number
> (SVN). For the hardware, these SVNs are referred to collectively as
> CPU_SVN, and for software referred as ISV_SVN. A TCB is considered up to
> date if all components of the TCB have SVNs greater than or equal to a
> threshold published by the author of the component.

Trusted Execution Environment
TEE

> A Trusted Execution Environment (TEE) is an environment where the code
> executed and the data accessed are isolated and protected in terms of
> confidentiality (no one has access to the data except the code running
> inside the TEE) and integrity (no one can change the code and its
> behavior).

Trusted Computing Base
TCB

> In context of `SGX` this has the usual meaning: the set of all
> components that are critical to security. Any vulnerability in TCB
> compromises security. Any problem outside TCB is not a \|~\| vulnerability,
> i.e. \|~\| should not compromise security.
>
> In context of Gramine there is also a \|~\| different meaning
> (`Thread Control Block`). Those two should not be confused.

Thread Control Structure
TCS

> <div class="todo">
>
> TBD
>
> </div>

</div>
