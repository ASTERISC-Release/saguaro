@echo off
rem Saguaro phase-0 spike: the probe enclave plus a section marked RWX (/SECTION:.gxrwx,ERW).
rem Run after build.cmd (uses its output folder).
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64 -no_logo || exit /b 1
cd /d "%~dp0out" || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\arm64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
set ENCLAVE_LIBS="%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\arm64\vertdll.lib" "%SDKLIB%um\arm64\bcrypt.lib" "%SDKLIB%ucrt_enclave\arm64\ucrt.lib"
armasm64 -nologo -o rwx_asm.obj "%~dp0rwx.asm" || exit /b 2
cl /nologo /c /W3 /MT /Zi /O1 /D_DEBUG /DPROBE_RWX /Fo"enclave_rwx.obj" /Fd"enclave_rwx.pdb" "%~dp0enclave.c" || exit /b 2
echo === link with /SECTION:.gxrwx,ERW
link /nologo /DLL /DEBUG /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /SECTION:.gxrwx,ERW /DEF:"%~dp0vbsenclave-rwx.def" /OUT:vbsenclave-rwx.dll enclave_rwx.obj probe_asm.obj rwx_asm.obj %ENCLAVE_LIBS%
echo link exit code %ERRORLEVEL%
if not exist vbsenclave-rwx.dll exit /b 3
dumpbin /headers vbsenclave-rwx.dll | findstr /i /c:".gxrwx" /c:"Execute Read Write" /c:"flags"
"%WindowsSdkVerBinPath%arm64\veiid.exe" vbsenclave-rwx.dll
echo veiid exit code %ERRORLEVEL%
"%WindowsSdkVerBinPath%arm64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" vbsenclave-rwx.dll > signtool-rwx.log 2>&1
echo signtool exit code %ERRORLEVEL%
