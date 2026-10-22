@echo off
rem Saguaro phase-0 spike: build, protect (veiid) and test-sign the probe enclave and its host.
rem Usage: build.cmd [extra cl defines, e.g. /DPROBE_THREADS=4]
setlocal
set VSDEV="%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
call %VSDEV% -arch=arm64 -host_arch=arm64 -no_logo || exit /b 1
set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%" || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\arm64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
set ENCLAVE_LIBS="%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\arm64\vertdll.lib" "%SDKLIB%um\arm64\bcrypt.lib" "%SDKLIB%ucrt_enclave\arm64\ucrt.lib"
echo === armasm64
armasm64 -nologo -o probe_asm.obj "%~dp0probe.asm" || exit /b 2
echo === cl (enclave)
cl /nologo /c /W3 /MT /Zi /O1 /D_DEBUG /DUNICODE /D_UNICODE %* /Fo"enclave.obj" /Fd"enclave.pdb" "%~dp0enclave.c" || exit /b 2
echo === link (enclave)
link /nologo /DLL /DEBUG /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0vbsenclave.def" /OUT:vbsenclave.dll enclave.obj probe_asm.obj %ENCLAVE_LIBS% || exit /b 3
echo === veiid
"%WindowsSdkVerBinPath%arm64\veiid.exe" vbsenclave.dll || exit /b 4
echo === signtool
"%WindowsSdkVerBinPath%arm64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" vbsenclave.dll > signtool.log 2>&1
set SIGNRC=%ERRORLEVEL%
echo signtool exit code: %SIGNRC% (2 = signed with a warning)
if %SIGNRC% NEQ 0 if %SIGNRC% NEQ 2 (type signtool.log & exit /b 5)
echo === cl+link (host)
cl /nologo /W3 /MT /Zi /O2 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /Fe"host.exe" "%~dp0host.cpp" /link /DEBUG onecore.lib || exit /b 6
echo === build done: %OUT%
