@echo off
rem Saguaro phase-0 spike (step g): link an enclave object compiled on Linux by clang
rem (out-clang\enclave_clang.obj, made by ../cross-build/xcompile.sh) with the MSVC linker; run build.cmd first.
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64 -no_logo || exit /b 1
cd /d "%~dp0out-clang" || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\arm64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
link /nologo /DLL /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0vbsenclave.def" /OUT:vbsenclave.dll enclave_clang.obj ..\out\elf_asm.obj "%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\arm64\vertdll.lib" "%SDKLIB%ucrt_enclave\arm64\ucrt.lib" || exit /b 3
dumpbin /loadconfig vbsenclave.dll | findstr /i /c:"enclave" /c:"Guard Flags"
"%WindowsSdkVerBinPath%arm64\veiid.exe" vbsenclave.dll || exit /b 4
"%WindowsSdkVerBinPath%arm64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" vbsenclave.dll > signtool.log 2>&1
if %ERRORLEVEL% GTR 2 (type signtool.log & exit /b 5)
copy /y ..\out\host.exe . >nul
host.exe 1
host.exe 2
