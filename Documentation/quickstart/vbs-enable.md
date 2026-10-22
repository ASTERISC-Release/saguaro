# Enabling VBS on a Windows machine

The `vbs` PAL runs applications inside a Windows *virtualization-based security* (VBS) enclave. This page lists the
one-time steps that prepare a Windows 11 machine (ARM64 or x64) for it. None of them is done by the Saguaro tools:
each changes the machine and is the administrator's decision. When the machine is ready, continue with
[vbs-windows.md](vbs-windows.md).

## 1. VBS running

Windows 11 with VBS running. Check in PowerShell:

    (Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard `
        -ClassName Win32_DeviceGuard).VirtualizationBasedSecurityStatus

The value `2` means running. VBS is turned on in *Windows Security* > *Device security* > *Core isolation* (memory
integrity) or by group policy, and needs a restart.

## 2. Test signing

Enclaves signed with a self-made certificate need test signing. As administrator, run
`bcdedit /set testsigning on` and restart; `bcdedit` then shows `testsigning Yes`. With Secure Boot on, `bcdedit` may
refuse the change ("protected by Secure Boot policy"); see Microsoft's documentation.

## 3. Visual Studio and the Windows SDK

Visual Studio 2022 or later with the *Desktop development with C++* workload (on ARM64 Windows with the ARM64 build
tools), and the Windows SDK 10.0.26100 or later, which provides `vertdll.lib`, `veiid.exe` and `signtool.exe`. The
tools find Visual Studio with `vswhere`; set `VS_INSTALL_DIR` to name another installation.

## 4. A signing certificate

A certificate in the current user's store with the code signing, enclave signing and enclave author key usages
(OIDs `1.3.6.1.5.5.7.3.3`, `1.3.6.1.4.1.311.76.57.1.15` and
`1.3.6.1.4.1.311.97.814040577.346743379.4783502.105532346`), as Microsoft's VBS enclave development guide describes.
For example:

    New-SelfSignedCertificate -CertStoreLocation Cert:\CurrentUser\My `
        -Subject "CN=TheDefaultTestEnclaveCertName" -KeyAlgorithm RSA -KeyLength 2048 `
        -KeyUsage DigitalSignature -Type Custom -TextExtension @(
        "2.5.29.37={text}1.3.6.1.5.5.7.3.3,1.3.6.1.4.1.311.76.57.1.15,1.3.6.1.4.1.311.97.814040577.346743379.4783502.105532346")

The tools use the subject `CN=TheDefaultTestEnclaveCertName`; pass `--cert-name` to `saguaro-vbs` for another one.

## 5. Smart App Control off

Smart App Control must be off. In enforcement mode it refuses enclave images signed with a self-made
certificate (`LoadEnclaveImageW` fails with 4551, and the code-integrity log names the `VerifiedAndReputableDesktop`
policy) and, without cloud lookups, new unsigned executables such as `pal_vbs_host.exe`. Turn it off in *Windows
Security* > *App & browser control* > *Smart App Control settings*. On some Windows builds it cannot be turned on again
without resetting Windows.

## Check the result

After installing the runtime in WSL (`scripts/install.sh --vbs`), `saguaro-vbs check` reports each of the steps above
that is missing: the architecture, VBS running, Visual Studio, the MSVC enclave libraries, the Windows SDK, `veiid.exe`
and `signtool.exe`, and the signing certificate. It does not report test signing or Smart App Control: read them with
`bcdedit` and in *Windows Security*.
