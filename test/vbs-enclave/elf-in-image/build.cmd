@echo off
rem Gramine phase-0 spike (step h): build the enclave that contains the Linux program.
rem Run linux/build.sh in WSL first (it writes linux\text.bin and linux\data.bin).
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64 -no_logo || exit /b 1
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%~dp0out" || exit /b 1
copy /y "%~dp0linux\text.bin" . >nul || exit /b 2
copy /y "%~dp0linux\data.bin" . >nul || exit /b 2
set VCLIB=%VCToolsInstallDir%lib\arm64
set SDKLIB=%WindowsSdkDir%Lib\%WindowsSDKVersion%
armasm64 -nologo -o elf_asm.obj "%~dp0elf.asm" || exit /b 2
cl /nologo /c /W3 /MT /Zi /O1 /Fo"enclave.obj" /Fd"enclave.pdb" "%~dp0enclave.c" || exit /b 2
link /nologo /DLL /DEBUG /NODEFAULTLIB /INTEGRITYCHECK /ENCLAVE /GUARD:MIXED /DEF:"%~dp0vbsenclave.def" /OUT:vbsenclave.dll enclave.obj elf_asm.obj "%VCLIB%\enclave\libcmt.lib" "%VCLIB%\enclave\libvcruntime.lib" "%SDKLIB%um\arm64\vertdll.lib" "%SDKLIB%ucrt_enclave\arm64\ucrt.lib" || exit /b 3
dumpbin /headers vbsenclave.dll | findstr /c:".gxelf" /c:"virtual address"
"%WindowsSdkVerBinPath%arm64\veiid.exe" vbsenclave.dll || exit /b 4
"%WindowsSdkVerBinPath%arm64\signtool.exe" sign /ph /fd SHA256 /n "TheDefaultTestEnclaveCertName" vbsenclave.dll > signtool.log 2>&1
if %ERRORLEVEL% GTR 2 (type signtool.log & exit /b 5)
cl /nologo /W3 /MT /O2 /Fe"host.exe" "%~dp0host.cpp" /link onecore.lib || exit /b 6
echo === build done
