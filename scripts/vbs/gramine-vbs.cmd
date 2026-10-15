@echo off
rem SPDX-License-Identifier: LGPL-3.0-or-later
rem Copyright (C) 2026 Gramine contributors
rem gramine-vbs on Windows: runs gramine-vbs.ps1 (next to this file) with Windows PowerShell.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0gramine-vbs.ps1" %*
exit /b %ERRORLEVEL%
