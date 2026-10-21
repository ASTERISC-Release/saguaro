# Manifest Key Catalog

This catalog indexes manifest keys and key families documented or used by this tree. It complements
the detailed shared, VBS, and SGX descriptions in [manifest-syntax.md](manifest-syntax.md) and
links to tree-specific VM, TDX, and SEV settings documented elsewhere.

**Applicability:** `Common` means the key is documented under common syntax. Experimental
features and architecture limits are noted in the linked documentation.

## Loader and process

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `loader.log_level`, `loader.log_file` | Common | Set log verbosity and optional log file. | [Log level](manifest-syntax.md#log-level) |
| `loader.entrypoint` | Common; VBS manifests specify it explicitly | Select the LibOS binary loaded by the PAL. | [Loader entrypoint](manifest-syntax.md#loader-entrypoint) |
| `libos.entrypoint` | Common | Select the application's initial executable. | [LibOS entrypoint](manifest-syntax.md#libos-entrypoint) |
| `loader.insecure__use_cmdline_argv`, `loader.argv`, `loader.argv_src_file` | Common | Select how the application's command-line arguments are provided. | [Command-line arguments](manifest-syntax.md#command-line-arguments) |
| `loader.insecure__use_host_env` | Common | Pass the host environment to the application. | [Environment variables](manifest-syntax.md#environment-variables) |
| `loader.env.<NAME>` | Common | Set or pass through one named environment variable; table fields are `value` and `passthrough`. | [Environment variables](manifest-syntax.md#environment-variables) |
| `loader.env_src_file` | Common | Load serialized environment variables from a file. | [Environment variables](manifest-syntax.md#environment-variables) |
| `loader.uid`, `loader.gid` | Common | Set the emulated initial user and group IDs. | [User ID and Group ID](manifest-syntax.md#user-id-and-group-id) |
| `loader.insecure__disable_aslr` | Common | Disable address-space layout randomization. | [Disabling ASLR](manifest-syntax.md#disabling-aslr) |
| `libos.check_invalid_pointers` | Common | Control validation of application pointers passed to system calls. | [Check invalid pointers](manifest-syntax.md#check-invalid-pointers) |
| `sys.stack.size` | Common | Set the per-thread stack size. | [Stack size](manifest-syntax.md#stack-size) |
| `sys.brk.max_size` | Common | Set the maximum program-break size per process. | [Program break size](manifest-syntax.md#program-break-brk-size) |
| `sys.insecure__allow_eventfd` | Common; pass-through security caveat for SGX | Select the host-backed eventfd implementation. | [Allowing host-based insecure eventfd](manifest-syntax.md#allowing-host-based-insecure-eventfd) |
| `sys.enable_sigterm_injection` | Common; SGX security caveat | Allow one-time external SIGTERM injection. | [External SIGTERM injection](manifest-syntax.md#external-sigterm-injection) |
| `sys.disallow_subprocesses` | Common | Prevent applications from creating child processes. | [Disallowing subprocesses](manifest-syntax.md#disallowing-subprocesses-fork) |

## Filesystem

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `fs.root.type`, `fs.root.uri` | Common | Configure the root filesystem mount. | [Root filesystem mount point](manifest-syntax.md#root-fs-mount-point) |
| `fs.mounts[]` | Common; mount-type support depends on the PAL | Mount host-backed, encrypted, untrusted shared-memory, or temporary filesystems. Fields include `type`, `path`, and `uri`. | [Filesystem mount points](manifest-syntax.md#fs-mount-points) |
| `fs.mounts[].host_symlinks` | `gramine-direct` only | Allow host symlink targets to be returned by `readlink()` under a chroot mount. | [FS mount points](manifest-syntax.md#fs-mount-points) |
| `fs.mounts[].key_name` | Encrypted mounts | Select the encryption key for a mount; defaults to `default`. | [Encrypted files](manifest-syntax.md#encrypted-files) |
| `fs.insecure__keys.<NAME>` | Encrypted mounts; built-in SGX sealing keys are SGX-specific | Supply an encryption key directly in the manifest. | [Encrypted files](manifest-syntax.md#encrypted-files) |
| `fs.start_dir` | Common | Set the initial working directory. | [Start directory](manifest-syntax.md#start-current-working-directory) |

## System and device access

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `sys.enable_extra_runtime_domain_names_conf` | Common | Generate hostname and resolver configuration from the host. | [Domain names configuration](manifest-syntax.md#domain-names-configuration) |
| `sys.rewrite_syscalls` | `gramine-direct` on AArch64 and RISC-V; VBS manifests set it to `false` | Rewrite trapped syscall sites to reduce repeated trap overhead. | [Syscall-site rewriting](manifest-syntax.md#rewriting-trapped-syscall-sites-aarch64-and-riscv64-direct-pal), [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `sys.ioctl_structs.<NAME>[]` | Common IOCTL configuration; SGX has additional memory-boundary considerations | Describe an IOCTL argument's memory layout. Fields include `name`, `alignment`, `size`, `unit`, `adjustment`, `direction`, `ptr`, `array_len`, and `onlyif`. | [Allowed IOCTLs](manifest-syntax.md#allowed-ioctls) |
| `sys.allowed_ioctls[]` | Common IOCTL configuration | Allow selected IOCTL request codes; fields are `request_code` and optional `struct`. | [Allowed IOCTLs](manifest-syntax.md#allowed-ioctls) |
| `sys.insecure__allow_raw_device_ioctl` | `gramine-direct` only; other PALs ignore it | Pass mounted-device IOCTLs directly to the host. | [Raw device IOCTLs](manifest-syntax.md#raw-device-ioctls-gramine-direct) |
| `sys.insecure__host_unix_sockets` | `gramine-direct` only; other PALs ignore it | Permit connections to listed host UNIX socket paths. | [Host UNIX sockets](manifest-syntax.md#host-unix-sockets-gramine-direct) |
| `sys.experimental__enable_flock` | Common | Enable the experimental `flock` system call. | [Experimental flock support](manifest-syntax.md#experimental-flock-bsd-style-locks-support) |

## VM launcher

These keys configure the `gramine-vm` launcher and relay, not the Linux PAL. The reference describes
the relay for x86_64 and AArch64.

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `vm.net.outbound` | `gramine-vm` | Allow or deny guest-initiated TCP and UDP connections. | [VM networking](manifest-syntax.md#vm-networking-gramine-vm) |
| `vm.net.deny` | `gramine-vm` | Deny destinations matching IP-network or protocol-port rules. | [VM networking](manifest-syntax.md#vm-networking-gramine-vm) |
| `vm.net.inbound[]` | `gramine-vm` | Forward host listener ports to guest TCP ports; fields include `guest`, `host`, and `bind`. | [VM networking](manifest-syntax.md#vm-networking-gramine-vm) |
| `vm.net.direct_vsock_ports` | `gramine-vm` | Map guest TCP ports directly to host AF_VSOCK ports, bypassing the relay. | [VM networking](manifest-syntax.md#vm-networking-gramine-vm) |

## Tree-specific VM and confidential-VM options

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `sys.experimental__multiprocess` | VM PAL; experimental, with architecture-specific limits | Process creation and related multi-process support in the guest. Default `true` on the x86_64 and AArch64 VM PAL (needs guest RAM above 4G on x86_64); `false` gives the single-process guest. | [VM process support](quickstart/vm.md#4-other-information) |
| `sys.experimental__assume_external_processes_alive` | Common LibOS option; used by VM/TDX integrations | Make `kill(pid, 0)` succeed when the target is outside Gramine and would otherwise be reported as absent. | [Implementation](../libos/src/sys/libos_sigaction.c) |
| `sys.experimental__shared_input_ring` | VM and TDX PALs; graphics use | Make the VM vsock driver use interrupt-driven polling when the graphics shared input ring is active. | dev docs: `graphics/` |
| `sys.experimental__enable_host_sysv_semaphores` | VM/TDX with graphics enabled and a host agent | Forward System V semaphore operations to the host agent. | [Graphics example](../CI-Examples/graphics/tf2/README.md) |
| `tdx.enclave_size` | `gramine-tdx` | Set guest memory size from the TDX manifest. | [VM size](quickstart/vm.md#4-other-information) |
| `tdx.trusted_files[]` | TDX | Verify listed files against hashes; entries contain `uri` and `sha256`. | [SGX trusted-file syntax](manifest-syntax.md#trusted-files), [implementation](../pal/src/host/vm-common/pal_common_tf.c) |
| `tdx.allowed_files[]` | TDX | Allow listed files or directories without hash verification. | [SGX allowed-file syntax](manifest-syntax.md#allowed-files), [implementation](../pal/src/host/vm-common/pal_common_tf.c) |
| `tdx.file_check_policy` | TDX | Select strict file checking or allow unknown files while logging them. | [SGX file-check policy](manifest-syntax.md#file-check-policy), [implementation](../pal/src/host/vm-common/pal_common_tf.c) |
| `sev.mode` | `gramine-sev` | Select SNP (default), plain SEV, or the native boot-path debug mode. | [SEV settings](quickstart/sev.md#2-example-hello-world), [launcher](../tools/gramine-vm.in) |
| `sev.policy` | `gramine-sev` | Set the guest policy passed to QEMU; defaults depend on the selected mode. | [SEV settings](quickstart/sev.md#2-example-hello-world), [launcher](../tools/gramine-vm.in) |

## VBS

The VBS-specific keys are for the Windows VBS PAL; other PALs ignore them.

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `vbs.packed_files[]` | VBS | Include application files in the signed enclave image. Fields include `path`, `uri`, and optional `exclude`. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.trusted_files[]` | VBS | Verify host files against SHA-256 hashes when opened; fields are `path` and `sha256`. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.allowed_files[]` | VBS | Allow unverified host files or directories. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.debug` | VBS | Set whether the enclave image allows debugging. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.enclave_size`, `vbs.max_threads` | VBS | Set the enclave address range and maximum concurrent threads. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.family_id`, `vbs.image_id`, `vbs.isv_svn` | VBS | Set enclave identity values included in attestation reports. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.x18_allow` | VBS on AArch64 | Allow listed functions to use register x18 in packed code. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.x18_skip_constructors` | VBS on AArch64 | Skip constructors for listed packed objects when checking x18 use. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |
| `vbs.build` | VBS | Run a build command before packing the enclave image. | [VBS syntax](manifest-syntax.md#vbs-syntax-windows-vbs-enclaves) |

## SGX

The SGX-specific keys are ignored when Gramine is not running with SGX.

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `sgx.debug` | SGX | Set whether the enclave is debug or production. | [SGX syntax](manifest-syntax.md#sgx-syntax) |
| `sgx.edmm_enable` | SGX | Enable dynamic memory management after enclave creation. | [EDMM](manifest-syntax.md#edmm) |
| `sgx.enclave_size` | SGX | Set the enclave size or, with EDMM, its maximum size. | [Enclave size](manifest-syntax.md#enclave-size) |
| `sgx.max_threads` | SGX | Set the thread-slot limit or preallocated slots, depending on EDMM. | [Number of threads](manifest-syntax.md#number-of-threads) |
| `sgx.insecure__rpc_thread_num` | SGX | Set the number of untrusted RPC threads for exitless operation. | [RPC threads](manifest-syntax.md#number-of-rpc-threads-exitless-feature) |
| `sgx.use_exinfo`, `sgx.insecure__allow_memfaults_without_exinfo` | SGX | Configure fault-address reporting and handling when EXINFO is unavailable. | [SGX EXINFO](manifest-syntax.md#sgx-exinfo) |
| `sgx.cpu_features.<FEATURE>` | SGX | Configure AVX, AVX512, AMX, MPX, or PKRU availability to the enclave. | [CPU features](manifest-syntax.md#optional-cpu-features-avx-avx512-amx-mpx-pkru) |
| `sgx.isvprodid`, `sgx.isvsvn` | SGX | Set product and security-version values in the enclave signature. | [ISV ID and SVN](manifest-syntax.md#isv-product-id-and-svn) |
| `sgx.seal_key.flags_mask`, `sgx.seal_key.xfrm_mask`, `sgx.seal_key.misc_mask` | SGX | Select enclave attributes used to derive sealing keys. | [Sealing key masks](manifest-syntax.md#attribute-masks-for-sgx-sealing-key-derivation) |
| `sgx.allowed_files[]` | SGX | Allow listed files, directories, or devices without integrity verification. | [Allowed files](manifest-syntax.md#allowed-files) |
| `sgx.trusted_files[]` | SGX | Verify listed files against hashes; table entries use `uri` and optional `sha256`. | [Trusted files](manifest-syntax.md#trusted-files) |
| `sgx.file_check_policy` | SGX | Select how unknown files and devices are authenticated. | [File check policy](manifest-syntax.md#file-check-policy) |
| `sgx.remote_attestation` | SGX | Select no remote attestation, EPID, or DCAP. | [Attestation](manifest-syntax.md#attestation-and-quotes) |
| `sgx.ra_client_linkable`, `sgx.ra_client_spid` | SGX with EPID | Configure EPID linkability and client SPID. | [Attestation](manifest-syntax.md#attestation-and-quotes) |
| `sgx.preheat_enclave` | SGX | Pre-fault heap pages during initialization. | [Pre-heating](manifest-syntax.md#pre-heating-enclave) |
| `sgx.enable_stats` | SGX | Enable SGX-specific statistics and debug/profiling support. | [SGX statistics](manifest-syntax.md#enabling-per-thread-and-process-wide-sgx-stats) |
| `sgx.profile.enable`, `sgx.profile.mode`, `sgx.profile.with_stack`, `sgx.profile.frequency` | SGX | Configure SGX profiling mode, events, stack data, and sample frequency. | [SGX profiling](manifest-syntax.md#sgx-profiling) |
| `sgx.vtune_profile` | SGX | Enable Intel VTune Profiler integration. | [VTune profiling](manifest-syntax.md#sgx-profiling-with-intel-vtune-profiler) |