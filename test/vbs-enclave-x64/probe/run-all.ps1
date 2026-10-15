# Copyright (C) 2026 The ASTERISC Research Lab
# Gramine x64 phase-0 spike: run every probe test (after build.cmd) and keep each output in
# results\<test>.txt; the contention test starts many fsspin processes at once.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File run-all.ps1 [-Only name,name] [-Procs 24]
param([string[]]$Only = @(), [int]$Procs = 24)
$ErrorActionPreference = 'Continue'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$out = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $PSScriptRoot 'out\host.exe'
Set-Location (Join-Path $PSScriptRoot 'out')
function T([string]$name, [string[]]$a, [int]$watchdogMs = 60000) {
    if ($Only.Count -and -not ($Only -contains $name)) { return }
    $env:PROBE_WATCHDOG_MS = "$watchdogMs"
    $t0 = Get-Date
    & $exe @a *> (Join-Path $out "$name.txt")
    $rc = $LASTEXITCODE
    $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    "exit=$rc ms=$ms" | Add-Content (Join-Path $out "$name.txt")
    "{0,-14} exit={1,-11} {2,7} ms  host.exe {3}" -f $name, $rc, $ms, ($a -join ' ')
}
T hostcpu   @('hostcpu')
T hostexc   @('hostexc')
T exec1     @('exec', '1')
T exec2     @('exec', '2') 20000
T exec3     @('exec', '3')
T basic     @('basic', '0x2FD')
T exec-nx   @('basic', '0x2') 20000
T write-code @('basic', '0x108') 20000
T fsbase    @('fsbase')
T fsocall   @('fsocall', '100000') 300000
T fsspin    @('fsspin', '4000000000') 120000
T redzone   @('redzone')
T offstack1 @('offstack', '1') 20000
T offstack2 @('offstack', '2') 20000
T offstack3 @('offstack', '3') 20000
T stack     @('stack')
T teb       @('teb')
T stackgrow64 @('stackgrow', '64')
T stackgrow900 @('stackgrow', '900')
T stackgrow2000 @('stackgrow', '2000')
T cfg       @('cfg') 20000
T cost      @('cost', '100000') 300000
T threads   @('threads', '16', '15')
T threads-nowait @('threads', '2', '3', '0')
T spin      @('spin', '16', '2000')
T tls       @('tls')
T api       @('api')
T hostread  @('hostread')
T rpm       @('rpm') 120000
T rpm-nodebug @('rpm', 'nodebug') 120000
T exccost   @('exccost', '10000')
T loaddata  @('loaddata')
T nodebug   @('nodebug')
if (-not $Only.Count -or ($Only -contains 'contention')) {
    # FS base and registers under contention: $Procs processes at once on 22 logical processors, half
    # spinning (fsspin), half doing OCALLs (fsocall)
    $env:PROBE_WATCHDOG_MS = '300000'
    $ps = 1..$Procs | ForEach-Object {
        $a = if ($_ % 2) { @('fsspin', '20000000000') } else { @('fsocall', '1000000') }
        Start-Process -FilePath $exe -ArgumentList $a -NoNewWindow -PassThru `
            -RedirectStandardOutput (Join-Path $out "contention-$_.txt")
    }
    $ps | Wait-Process
    Get-ChildItem (Join-Path $out 'contention-*.txt') | Select-String 'spin:|ocall:' | ForEach-Object { $_.Line } |
        Set-Content (Join-Path $out 'contention.txt')
    "contention     $Procs processes; summary in results\contention.txt"
}
