@echo off
rem Runs the step-0 host from its output directory. Usage: run.cmd [arm64|x64]
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=arm64
cd /d "%~dp0out-%ARCH%" || exit /b 1
host.exe
echo host-exit=%ERRORLEVEL%
