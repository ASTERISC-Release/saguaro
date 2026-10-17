# Building the Windows VBS-enclave PAL

The `vbs` PAL targets AArch64 Windows (see `Documentation/devel/vbs-enclave/PLAN.md`, decisions D1-D3
and the design of the hello-world path). An enclave image holds two kinds of code:

- the **Windows enclave runtime** (`pal/src/host/vbs/enclave/`) and the **host** executable
  (`pal/src/host/vbs/host/`): Windows ARM64 code, compiled on Linux with
  `clang --target=aarch64-pc-windows-msvc` and linked, `veiid`-processed and test-signed on Windows;
- **packed AArch64 Linux ELF objects**: the vbs PAL proper (`libpal.so`), the LibOS
  (`libsysdb.so`), the vDSO and the application, built by meson with `-Dvbs=enabled` on an aarch64
  machine (the repository's build container), and the manifest, all placed into the image by
  `pack.py`.

`gramine-vbs` (in this folder; `Documentation/vbs.md`) does all of the steps below from a
manifest: `gramine-vbs run app.manifest`. `gramine-vbs.ps1` is its Windows-only half. The steps by
hand (the commands used for the phase-2 hello world):

1. **Linux ELF objects** (aarch64 build container):

   ```sh
   meson setup build-vbs -Ddirect=disabled -Dvbs=enabled -Dlibc=none -Dtests=disabled \
       -Dbuildtype=release -Dgraphics=disabled
   ninja -C build-vbs
   pal/src/host/vbs/test/hello/build.sh $PWD/build-vbs/hello     # static PIE, -ffixed-x18
   ```

2. **Pack** (`pack.py`, any machine with Python 3): rewrites each `svc #0` of the application and
   vDSO into a branch to a stub, keeps the original file bytes for reads, and generates the blob and
   the object table.

   ```sh
   scripts/vbs/pack.py --out PACK --obj pal:libpal:libpal.so \
       --obj pal:libsysdb:libsysdb.so:/libsysdb.so --obj user:vdso:vdso.so:/gramine/vdso.so \
       --obj user:app:hello:/hello --file manifest:hello.manifest:/hello.manifest --no-svc libpal \
       --x18-allow libsysdb:libos_syscall_entry,_return_from_syscall
   ```

3. **Windows objects** (`build-objs.sh`, on Linux): needs the Windows SDK and MSVC headers copied
   into a scratch folder pointed to by `HDR` (see the script); with `PACK` it also builds the
   packed-object loader and assembles the blob.

   ```sh
   HDR=/path/to/winhdr OUT=build-vbs/obj PACK=PACK scripts/vbs/build-objs.sh
   ```

4. **Link and sign** (`link-sign.cmd`, on Windows, in the object folder with the `.def` file):
   `link /ENCLAVE` + `veiid` + `signtool` with the current-user test certificate
   `CN=TheDefaultTestEnclaveCertName`, and the host executable.

5. **Run** on Windows: `pal_vbs_host.exe pal_vbs_enclave.dll 1 boot` runs the application;
   `pal_vbs_host.exe pal_vbs_enclave.dll 1` runs the PAL-level self-test.

**On one Windows machine** (no second Linux machine; `Documentation/devel/vbs-enclave/PLAN-windows-build.md`):
steps 1 and 2 run inside a WSL2 Ubuntu 24.04 aarch64 distribution (`wsl-setup.sh` checks and installs
its tools, with apt or, without sudo, into a user prefix). `gramine-vbs.cmd` (`gramine-vbs.ps1`) is
`gramine-vbs` for that machine: it copies the checkout into WSL, runs the Linux side there
(`wsl-build.sh manifest`) and links, signs and runs on Windows. `build-enclave.ps1` does steps 3 to
5 for a pack made by hand in WSL (`wsl-build.sh objs`: the objects with clang inside WSL, against
this machine's SDK and MSVC headers).
