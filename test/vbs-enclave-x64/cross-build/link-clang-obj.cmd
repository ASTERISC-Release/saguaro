@echo off
rem Gramine x64 phase-0 spike (step g): link enclave objects compiled on Linux by clang
rem (xcompile.sh) with the MSVC linker, veiid and sign them. Run ..\probe\build.cmd and
rem ..\elf-in-image\build.cmd first (their ml64 objects and elf_blob.obj are reused), and copy
rem enclave_clang.obj and elf_enclave_clang.obj into out\ here.
rem Copyright (C) 2026 The ASTERISC Research Lab
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo || exit /b 1
cd /d "%~dp0out" || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\x64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
set LIBS="%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\x64\vertdll.lib" "%SDKLIB%um\x64\bcrypt.lib" "%SDKLIB%ucrt_enclave\x64\ucrt.lib"
echo === probe enclave (clang object)
link /nologo /DLL /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0..\probe\vbsenclave.def" /OUT:probe-clang.dll enclave_clang.obj ..\..\probe\out\probe_asm.obj ..\..\probe\out\probe_sys.obj %LIBS% || exit /b 3
dumpbin /nologo /loadconfig probe-clang.dll | findstr /i /c:"enclave" /c:"Guard Flags"
"%WindowsSdkVerBinPath%x64\veiid.exe" probe-clang.dll || exit /b 4
"%WindowsSdkVerBinPath%x64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" probe-clang.dll > signtool-probe.log 2>&1
echo signtool exit code %ERRORLEVEL%
echo === elf enclave (clang object)
link /nologo /DLL /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0..\elf-in-image\vbsenclave.def" /OUT:elf-clang.dll elf_enclave_clang.obj ..\..\elf-in-image\out\elf_blob.obj ..\..\elf-in-image\out\thunk.obj %LIBS% || exit /b 3
dumpbin /nologo /headers elf-clang.dll | findstr /c:".gxelf" /c:"virtual address ("
"%WindowsSdkVerBinPath%x64\veiid.exe" elf-clang.dll || exit /b 4
"%WindowsSdkVerBinPath%x64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" elf-clang.dll > signtool-elf.log 2>&1
echo signtool exit code %ERRORLEVEL%
echo === done (run: copy probe-clang.dll ..\..\probe\out\vbsenclave.dll, or elf-clang.dll for the elf host)
