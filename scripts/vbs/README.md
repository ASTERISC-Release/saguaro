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

Steps (the commands used for the phase-2 hello world):

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
       --obj user:app:hello:/hello --file manifest:hello.manifest:/hello.manifest --no-svc libpal
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
