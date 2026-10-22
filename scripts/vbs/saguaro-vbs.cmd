@echo off
rem SPDX-License-Identifier: LGPL-3.0-or-later
rem Copyright (C) 2026 The ASTERISC Research Lab
rem saguaro-vbs on Windows: runs saguaro-vbs.ps1 (next to this file) with Windows PowerShell.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0saguaro-vbs.ps1" %*
exit /b %ERRORLEVEL%
