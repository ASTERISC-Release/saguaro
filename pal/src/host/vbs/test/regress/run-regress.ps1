# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Runs the cases of cases.txt (test|arg|arg...) against the signed image <Name>.dll in Dir, one host
# process per case, a watchdog of Timeout seconds; prints one line per case and a summary.
param([string]$Dir, [string]$Name = "regress", [int]$Timeout = 60)
Set-Location $Dir
$root = Join-Path $Dir "root"
$logs = Join-Path $Dir "reglogs"
New-Item -ItemType Directory -Force -Path $logs, "$root\tests\tmp", "$root\tests\tmp_enc" | Out-Null
$pass = 0; $n = 0; $counts = @{}
foreach ($line in Get-Content (Join-Path $Dir "cases.txt")) {
    if (-not $line.Trim()) { continue }
    $parts = $line.Split('|')
    $test = $parts[0]
    $n++
    $counts[$test] = 1 + [int]$counts[$test]
    $tag = "$test-$($counts[$test])"
    # a clean tmp\ for every case, and the suite's host links for symlink_host
    Remove-Item -Recurse -Force "$root\tests\tmp\*" -ErrorAction SilentlyContinue
    if ($test -eq "symlink_host") {
        Set-Content -NoNewline -Path "$root\tests\tmp\symlink_host_target" -Value "x"
        New-Item -ItemType SymbolicLink -Path "$root\tests\tmp\symlink_host_live" -Target "symlink_host_target" | Out-Null
        New-Item -ItemType SymbolicLink -Path "$root\tests\tmp\symlink_host_dangling" -Target "symlink_host_missing" | Out-Null
    }
    $argl = @("$Name.dll", "0", "boot", $root) + ($parts | ForEach-Object { if ($_ -eq "") { '""' } else { $_ } })
    $t0 = Get-Date
    $p = Start-Process -FilePath ".\pal_vbs_host.exe" -ArgumentList $argl -NoNewWindow -PassThru `
        -RedirectStandardOutput "$logs\$tag.out" -RedirectStandardError "$logs\$tag.err"
    $null = $p.Handle  # keeps the process handle, so that ExitCode is available afterwards
    $done = $p.WaitForExit($Timeout * 1000)
    if (-not $done) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; $code = "hang" }
    else { $code = $p.ExitCode }
    $secs = ((Get-Date) - $t0).TotalSeconds
    $ok = ($code -eq 0) -and (Select-String -Quiet -Path "$logs\$tag.out" -Pattern "TEST OK")
    if ($ok) { $pass++ }
    "{0,-40} {1,-6} exit={2} {3:N1}s" -f $line, $(if ($ok) { "pass" } else { "FAIL" }), $code, $secs
}
"passed $pass of $n cases"
