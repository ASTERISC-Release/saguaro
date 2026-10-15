@echo off
rem SPDX-License-Identifier: LGPL-3.0-or-later
rem Copyright (C) 2026 Gramine contributors
rem
rem Phase 1 Windows half of the VBS-enclave PAL build: link the clang-compiled enclave objects into
rem a signed enclave DLL, then build the host executable. Run from a folder that holds the three
rem enclave objects (pal_vbs_enclave.obj, pal_vbs_selftest.obj, pal_vbs_boundary.obj) and the host
rem object (pal_vbs_host.obj) produced by scripts/vbs/build-objs.sh on Linux. Links with the MSVC
rem enclave CRT and the Windows SDK enclave libraries, protects with veiid and test-signs with the
rem current-user certificate CN=TheDefaultTestEnclaveCertName (or the subject name in VBS_CERT_NAME).
rem
rem If the folder also holds pal_vbs_packed.obj and pal_vbs_packed_blob.obj (build-objs.sh with PACK),
rem they are linked in: the in-enclave loader and the packed objects in the .gxpack section.
rem
rem Usage: link-sign.cmd            (run inside the object directory)
setlocal
rem Visual Studio: the newest installation with the C++ tools (vswhere), or VS_INSTALL_DIR
if "%VS_INSTALL_DIR%"=="" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS_INSTALL_DIR=%%i
if "%VS_INSTALL_DIR%"=="" (echo no Visual Studio found & exit /b 1)
call "%VS_INSTALL_DIR%\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64 -no_logo || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\arm64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
set ENCLAVE_LIBS="%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\arm64\vertdll.lib" "%SDKLIB%um\arm64\bcrypt.lib" "%SDKLIB%ucrt_enclave\arm64\ucrt.lib"

set DEF=%~dp0..\..\pal\src\host\vbs\enclave\pal_vbs_enclave.def
if exist pal_vbs_enclave.def set DEF=pal_vbs_enclave.def
set PACKED_OBJS=
if exist pal_vbs_packed.obj set PACKED_OBJS=pal_vbs_packed.obj pal_vbs_packed_blob.obj

echo === link (enclave DLL)
link /nologo /DLL /DEBUG /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED ^
    /DEF:"%DEF%" /OUT:pal_vbs_enclave.dll ^
    pal_vbs_enclave.obj pal_vbs_boot.obj pal_vbs_selftest.obj pal_vbs_boundary.obj %PACKED_OBJS% %ENCLAVE_LIBS% || exit /b 3

echo === image layout (keep an eye on section addresses for the future packer)
dumpbin /headers pal_vbs_enclave.dll | findstr /c:"virtual address" /c:"name" /c:"Size of image"

echo === veiid
"%WindowsSdkVerBinPath%arm64\veiid.exe" pal_vbs_enclave.dll || exit /b 4

echo === signtool (test cert)
if "%VBS_CERT_NAME%"=="" set VBS_CERT_NAME=TheDefaultTestEnclaveCertName
"%WindowsSdkVerBinPath%arm64\signtool.exe" sign /ph /fd SHA256 /n "%VBS_CERT_NAME%" pal_vbs_enclave.dll > signtool.log 2>&1
set SIGNRC=%ERRORLEVEL%
echo signtool exit code: %SIGNRC% (2 = signed with a warning)
if %SIGNRC% NEQ 0 if %SIGNRC% NEQ 2 (type signtool.log & exit /b 5)

echo === link (host exe)
link /nologo /DEBUG /OUT:pal_vbs_host.exe pal_vbs_host.obj pal_vbs_host_io.obj pal_vbs_verify.obj onecore.lib ws2_32.lib libcmt.lib || exit /b 6
echo === build done
