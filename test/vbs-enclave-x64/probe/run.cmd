@echo off
rem Runs one probe test from the output directory. Usage: run.cmd <test> [args]
rem Tests: basic [phases], exec <1|2|3>, fsbase, fsocall [n], fsspin [iters], redzone [iters],
rem offstack <1|2|3>, stack, cfg, cost [n], threads <tc> <m> [wait], spin <m> <ms>, tls, api [blob],
rem hostread, loaddata, nodebug, hostcpu [iters], hostexc [iters] (host only, no enclave).
cd /d "%~dp0out" || exit /b 1
host.exe %*
echo host-exit=%ERRORLEVEL%
