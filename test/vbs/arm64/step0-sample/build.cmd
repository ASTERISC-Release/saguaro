@echo off
rem Saguaro phase-0 spike, step 0: build, protect (veiid) and test-sign the sample enclave and its
rem host with explicit compiler and linker commands. Usage: build.cmd [arm64|x64]
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=arm64
set VSDEV="%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
call %VSDEV% -arch=%ARCH% -host_arch=arm64 -no_logo || exit /b 1
set OUT=%~dp0out-%ARCH%
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%" || exit /b 1
set VCLIB=%VCToolsInstallDir%lib\%ARCH%
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
set ENCLAVE_LIBS="%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\%ARCH%\vertdll.lib" "%SDKLIB%um\%ARCH%\bcrypt.lib" "%SDKLIB%ucrt_enclave\%ARCH%\ucrt.lib"
echo === cl (enclave)
cl /nologo /c /W3 /sdl /permissive- /MT /Zi /D_DEBUG /D_CONSOLE /DUNICODE /D_UNICODE /Fo"enclave.obj" /Fd"enclave.pdb" "%~dp0enclave.c" || exit /b 2
echo === link (enclave)
link /nologo /DLL /DEBUG /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0vbsenclave.def" /OUT:vbsenclave.dll enclave.obj %ENCLAVE_LIBS% || exit /b 3
echo === veiid
"%WindowsSdkVerBinPath%%ARCH%\veiid.exe" vbsenclave.dll || exit /b 4
echo === signtool
"%WindowsSdkVerBinPath%%ARCH%\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" vbsenclave.dll
set SIGNRC=%ERRORLEVEL%
echo signtool exit code: %SIGNRC% (2 = signed with a warning)
if %SIGNRC% NEQ 0 if %SIGNRC% NEQ 2 exit /b 5
echo === cl+link (host)
cl /nologo /W3 /sdl /permissive- /MT /Zi /std:c++17 /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS /Fe"host.exe" "%~dp0host.cpp" /link /DEBUG onecore.lib || exit /b 6
echo === build done: %OUT%
