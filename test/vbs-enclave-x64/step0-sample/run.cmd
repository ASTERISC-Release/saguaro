@echo off
rem Runs the step-0 host from its output directory. Usage: run.cmd [x64]
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
cd /d "%~dp0out-%ARCH%" || exit /b 1
host.exe
echo host-exit=%ERRORLEVEL%
