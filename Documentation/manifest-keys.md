# Manifest Key Catalog

This catalog indexes manifest keys and key families documented or used by this tree. The keys inherited from
Gramine (`loader.*`, `libos.*`, `sys.*`, `fs.*`, `sgx.*`) are described in detail in
[Gramine's manifest documentation](https://gramine.readthedocs.io/en/latest/manifest-syntax.html); the key
families added by this tree (`vm.*`, `vbs.*`, `tdx.*`, `sev.*`, and the `sys.*` keys named below) are described in the
Purpose column and in the guides linked in the Details column.

**Applicability:** `Common` means the key is documented under common syntax. Experimental
features and architecture limits are noted in the guides.

## Loader and process

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `loader.log_level`, `loader.log_file` | Common | Set log verbosity and optional log file. | |
| `loader.entrypoint` | Common; VBS manifests specify it explicitly | Select the LibOS binary loaded by the PAL. | |
| `libos.entrypoint` | Common | Select the application's initial executable. | |
| `loader.insecure__use_cmdline_argv`, `loader.argv`, `loader.argv_src_file` | Common | Select how the application's command-line arguments are provided. | |
| `loader.insecure__use_host_env` | Common | Pass the host environment to the application. | |
| `loader.env.<NAME>` | Common | Set or pass through one named environment variable; table fields are `value` and `passthrough`. | |
| `loader.env_src_file` | Common | Load serialized environment variables from a file. | |
| `loader.uid`, `loader.gid` | Common | Set the emulated initial user and group IDs. | |
| `loader.insecure__disable_aslr` | Common | Disable address-space layout randomization. | |
| `libos.check_invalid_pointers` | Common | Control validation of application pointers passed to system calls. | |
| `sys.stack.size` | Common | Set the per-thread stack size. | |
| `sys.brk.max_size` | Common | Set the maximum program-break size per process. | |
| `sys.insecure__allow_eventfd` | Common; pass-through security caveat for SGX | Select the host-backed eventfd implementation. | |
| `sys.enable_sigterm_injection` | Common; SGX security caveat | Allow one-time external SIGTERM injection. | |
| `sys.disallow_subprocesses` | Common | Prevent applications from creating child processes. | |

## Filesystem

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `fs.root.type`, `fs.root.uri` | Common | Configure the root filesystem mount. | |
| `fs.mounts[]` | Common; mount-type support depends on the PAL | Mount host-backed, encrypted, untrusted shared-memory, or temporary filesystems. Fields include `type`, `path`, and `uri`. | |
| `fs.mounts[].host_symlinks` | `saguaro-direct` only | Allow host symlink targets to be returned by `readlink()` under a chroot mount. | |
| `fs.mounts[].key_name` | Encrypted mounts | Select the encryption key for a mount; defaults to `default`. | |
| `fs.insecure__keys.<NAME>` | Encrypted mounts; built-in SGX sealing keys are SGX-specific | Supply an encryption key directly in the manifest. | |
| `fs.start_dir` | Common | Set the initial working directory. | |

## System and device access

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `sys.enable_extra_runtime_domain_names_conf` | Common | Generate hostname and resolver configuration from the host. | |
| `sys.rewrite_syscalls` | `saguaro-direct` on AArch64 and RISC-V; VBS manifests set it to `false` | Rewrite trapped syscall sites to reduce repeated trap overhead. | |
| `sys.ioctl_structs.<NAME>[]` | Common IOCTL configuration; SGX has additional memory-boundary considerations | Describe an IOCTL argument's memory layout. Fields include `name`, `alignment`, `size`, `unit`, `adjustment`, `direction`, `ptr`, `array_len`, and `onlyif`. | |
| `sys.allowed_ioctls[]` | Common IOCTL configuration | Allow selected IOCTL request codes; fields are `request_code` and optional `struct`. | |
| `sys.insecure__allow_raw_device_ioctl` | `saguaro-direct` only; other PALs ignore it | Pass mounted-device IOCTLs directly to the host. | |
| `sys.insecure__host_unix_sockets` | `saguaro-direct` only; other PALs ignore it | Permit connections to listed host UNIX socket paths. | |
| `sys.experimental__enable_flock` | Common | Enable the experimental `flock` system call. | |

## VM launcher

These keys configure the `saguaro-vm` launcher and relay, not the Linux PAL. The reference describes
the relay for x86_64 and AArch64.

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `vm.net.outbound` | `saguaro-vm` | Allow or deny guest-initiated TCP and UDP connections. | |
| `vm.net.deny` | `saguaro-vm` | Deny destinations matching IP-network or protocol-port rules. | |
| `vm.net.inbound[]` | `saguaro-vm` | Forward host listener ports to guest TCP ports; fields include `guest`, `host`, and `bind`. | |
| `vm.net.direct_vsock_ports` | `saguaro-vm` | Map guest TCP ports directly to host AF_VSOCK ports, bypassing the relay. | |

## Tree-specific VM and confidential-VM options

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `sys.experimental__multiprocess` | VM PAL; experimental, with architecture-specific limits | Process creation and related multi-process support in the guest. Default `true` on the x86_64 and AArch64 VM PAL (needs guest RAM above 4G on x86_64); `false` gives the single-process guest. | [VM process support](quickstart/vm.md#4-other-information) |
| `sys.experimental__assume_external_processes_alive` | Common LibOS option; used by VM/TDX integrations | Make `kill(pid, 0)` succeed when the target is outside Saguaro and would otherwise be reported as absent. | [Implementation](../libos/src/sys/libos_sigaction.c) |
| `sys.experimental__shared_input_ring` | VM and TDX PALs; graphics use | Make the VM vsock driver use interrupt-driven polling when the graphics shared input ring is active. | dev docs: `graphics/` |
| `sys.experimental__enable_host_sysv_semaphores` | VM/TDX with graphics enabled and a host agent | Forward System V semaphore operations to the host agent. | [Graphics example](../CI-Examples/graphics/tf2/README.md) |
| `tdx.enclave_size` | `saguaro-tdx` | Set guest memory size from the TDX manifest. | [VM size](quickstart/vm.md#4-other-information) |
| `tdx.trusted_files[]` | TDX | Verify listed files against hashes; entries contain `uri` and `sha256`. | [implementation](../pal/src/host/vm-common/pal_common_tf.c) |
| `tdx.allowed_files[]` | TDX | Allow listed files or directories without hash verification. | [implementation](../pal/src/host/vm-common/pal_common_tf.c) |
| `tdx.file_check_policy` | TDX | Select strict file checking or allow unknown files while logging them. | [implementation](../pal/src/host/vm-common/pal_common_tf.c) |
| `sev.mode` | `saguaro-sev` | Select SNP (default), plain SEV, or the native boot-path debug mode. | [SEV settings](quickstart/sev.md#2-example-hello-world), [launcher](../tools/saguaro-vm.in) |
| `sev.policy` | `saguaro-sev` | Set the guest policy passed to QEMU; defaults depend on the selected mode. | [SEV settings](quickstart/sev.md#2-example-hello-world), [launcher](../tools/saguaro-vm.in) |

## VBS

The VBS-specific keys are for the Windows VBS PAL; other PALs ignore them.

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `vbs.packed_files[]` | VBS | Include application files in the signed enclave image. Fields include `path`, `uri`, and optional `exclude`. | |
| `vbs.trusted_files[]` | VBS | Verify host files against SHA-256 hashes when opened; fields are `path` and `sha256`. | |
| `vbs.allowed_files[]` | VBS | Allow unverified host files or directories. | |
| `vbs.debug` | VBS | Set whether the enclave image allows debugging. | |
| `vbs.enclave_size`, `vbs.max_threads` | VBS | Set the enclave address range and maximum concurrent threads. | |
| `vbs.family_id`, `vbs.image_id`, `vbs.isv_svn` | VBS | Set enclave identity values included in attestation reports. | |
| `vbs.x18_allow` | VBS on AArch64 | Allow listed functions to use register x18 in packed code. | |
| `vbs.x18_skip_constructors` | VBS on AArch64 | Skip constructors for listed packed objects when checking x18 use. | |
| `vbs.build` | VBS | Run a build command before packing the enclave image. | |

## SGX

The SGX-specific keys are ignored when Saguaro is not running with SGX.

| Key | Applies to | Purpose | Details |
|---|---|---|---|
| `sgx.debug` | SGX | Set whether the enclave is debug or production. | |
| `sgx.edmm_enable` | SGX | Enable dynamic memory management after enclave creation. | |
| `sgx.enclave_size` | SGX | Set the enclave size or, with EDMM, its maximum size. | |
| `sgx.max_threads` | SGX | Set the thread-slot limit or preallocated slots, depending on EDMM. | |
| `sgx.insecure__rpc_thread_num` | SGX | Set the number of untrusted RPC threads for exitless operation. | |
| `sgx.use_exinfo`, `sgx.insecure__allow_memfaults_without_exinfo` | SGX | Configure fault-address reporting and handling when EXINFO is unavailable. | |
| `sgx.cpu_features.<FEATURE>` | SGX | Configure AVX, AVX512, AMX, MPX, or PKRU availability to the enclave. | |
| `sgx.isvprodid`, `sgx.isvsvn` | SGX | Set product and security-version values in the enclave signature. | |
| `sgx.seal_key.flags_mask`, `sgx.seal_key.xfrm_mask`, `sgx.seal_key.misc_mask` | SGX | Select enclave attributes used to derive sealing keys. | |
| `sgx.allowed_files[]` | SGX | Allow listed files, directories, or devices without integrity verification. | |
| `sgx.trusted_files[]` | SGX | Verify listed files against hashes; table entries use `uri` and optional `sha256`. | |
| `sgx.file_check_policy` | SGX | Select how unknown files and devices are authenticated. | |
| `sgx.remote_attestation` | SGX | Select no remote attestation, EPID, or DCAP. | |
| `sgx.ra_client_linkable`, `sgx.ra_client_spid` | SGX with EPID | Configure EPID linkability and client SPID. | |
| `sgx.preheat_enclave` | SGX | Pre-fault heap pages during initialization. | |
| `sgx.enable_stats` | SGX | Enable SGX-specific statistics and debug/profiling support. | |
| `sgx.profile.enable`, `sgx.profile.mode`, `sgx.profile.with_stack`, `sgx.profile.frequency` | SGX | Configure SGX profiling mode, events, stack data, and sample frequency. | |
| `sgx.vtune_profile` | SGX | Enable Intel VTune Profiler integration. | |