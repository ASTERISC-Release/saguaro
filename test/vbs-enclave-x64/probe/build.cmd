@echo off
rem Gramine x64 phase-0 spike: build, protect (veiid) and test-sign the probe enclave and its host
rem on an x64 machine. Usage: build.cmd [OUTDIR-SUFFIX [extra cl defines, e.g. /DPROBE_STRICT]]
rem Copyright (C) 2026 The ASTERISC Research Lab
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo || exit /b 1
set OUT=%~dp0out%1
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%" || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\x64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
set ENCLAVE_LIBS="%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\x64\vertdll.lib" "%SDKLIB%um\x64\bcrypt.lib" "%SDKLIB%ucrt_enclave\x64\ucrt.lib"
echo === ml64
ml64 /nologo /c /Zi /Fo probe_asm.obj "%~dp0probe.asm" || exit /b 2
ml64 /nologo /c /Zi /Fo probe_sys.obj "%~dp0probe_sys.asm" || exit /b 2
echo === cl (enclave)
cl /nologo /c /W3 /MT /Zi /O1 /guard:cf /D_DEBUG /DUNICODE /D_UNICODE %2 %3 %4 /Fo"enclave.obj" /Fd"enclave.pdb" "%~dp0enclave.c" || exit /b 2
echo === link (enclave)
link /nologo /DLL /DEBUG /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0vbsenclave.def" /OUT:vbsenclave.dll enclave.obj probe_asm.obj probe_sys.obj %ENCLAVE_LIBS% || exit /b 3
echo === veiid
"%WindowsSdkVerBinPath%x64\veiid.exe" vbsenclave.dll || exit /b 4
echo === signtool
"%WindowsSdkVerBinPath%x64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" vbsenclave.dll > signtool.log 2>&1
set SIGNRC=%ERRORLEVEL%
echo signtool exit code: %SIGNRC% (2 = signed with a warning)
if %SIGNRC% NEQ 0 if %SIGNRC% NEQ 2 (type signtool.log & exit /b 5)
echo === cl+link (host)
cl /nologo /W3 /MT /Zi /O2 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /Fe"host.exe" "%~dp0host.cpp" probe_asm.obj /link /DEBUG onecore.lib || exit /b 6
dumpbin /nologo /headers vbsenclave.dll | findstr /i /c:"gxcode" /c:"Execute Read"
echo === build done: %OUT%
