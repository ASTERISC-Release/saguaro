# Building the Windows VBS-enclave PAL (phase 1 skeleton)

The `vbs` PAL targets AArch64 Windows and is built with the Windows/clang toolchain, not meson
(`-Dvbs=enabled` only wires the option; meson produces no vbs artifacts). The build has two halves
(see `Documentation/devel/vbs-enclave/PLAN.md`, decision D3):

1. **Linux half** (`build-objs.sh`): compile the enclave sources and the host source with
   `clang --target=aarch64-pc-windows-msvc`, and assemble the AArch64 boundary thunk. Needs the
   Windows SDK and MSVC headers copied into a scratch folder pointed to by `HDR`:

   ```sh
   HDR=/path/to/winhdr OUT=build-vbs/obj scripts/vbs/build-objs.sh
   ```

2. **Windows half** (`link-sign.cmd`): link the objects into a signed enclave DLL
   (`link /ENCLAVE` + `veiid` + `signtool` with the current-user test certificate
   `CN=TheDefaultTestEnclaveCertName`) and build the host executable. Run it from the object
   directory on the Windows machine.

Then run `pal_vbs_host.exe pal_vbs_enclave.dll 1`. Everything may instead be compiled and linked on
Windows with MSVC (`cl`), which avoids copying headers; phase-1 measurements of both paths are in
`Documentation/devel/vbs-enclave/TESTING.md`.
