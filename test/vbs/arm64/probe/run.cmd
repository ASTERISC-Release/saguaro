@echo off
rem Runs one probe test from the output directory. Usage: run.cmd <test> [args]
cd /d "%~dp0out" || exit /b 1
host.exe %*
echo host-exit=%ERRORLEVEL%
