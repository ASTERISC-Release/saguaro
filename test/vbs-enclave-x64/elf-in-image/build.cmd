@echo off
rem Gramine x64 phase-0 spike (step h): build the enclave that contains the x86_64 Linux program.
rem Run linux/build.sh <DATA_ADDR> in WSL first (it writes linux\elf_blob.c and linux\elf_blob.h);
rem this script prints the distance between .gxelft and .gxelfd that DATA_ADDR must equal.
rem Copyright (C) 2026 The ASTERISC Research Lab
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo || exit /b 1
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%~dp0out" || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\x64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
ml64 /nologo /c /Fo thunk.obj "%~dp0thunk.asm" || exit /b 2
cl /nologo /c /W3 /MT /Zi /O1 /I"%~dp0linux" /Fo"enclave.obj" /Fd"enclave.pdb" "%~dp0enclave.c" || exit /b 2
cl /nologo /c /W3 /MT /O1 /Fo"elf_blob.obj" "%~dp0linux\elf_blob.c" || exit /b 2
link /nologo /DLL /DEBUG /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0vbsenclave.def" /OUT:vbsenclave.dll enclave.obj elf_blob.obj thunk.obj "%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\x64\vertdll.lib" "%SDKLIB%ucrt_enclave\x64\ucrt.lib" || exit /b 3
dumpbin /nologo /headers vbsenclave.dll | findstr /c:".gxelf" /c:"virtual address (" /c:"flags"
"%WindowsSdkVerBinPath%x64\veiid.exe" vbsenclave.dll || exit /b 4
"%WindowsSdkVerBinPath%x64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" vbsenclave.dll > signtool.log 2>&1
if %ERRORLEVEL% GTR 2 (type signtool.log & exit /b 5)
cl /nologo /W3 /MT /O2 /Fe"host.exe" "%~dp0host.cpp" /link onecore.lib || exit /b 6
echo === build done
