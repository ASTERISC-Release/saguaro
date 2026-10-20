# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# gramine-vbs.ps1: the Windows-side runner of the VBS enclave tooling (Documentation/vbs.md). It
# launches a signed enclave package, checks the Windows side of the machine and verifies attestation
# reports. It does not build: applications are built and packaged in WSL with `make VBS=1`, and
# `gramine-vbs <program>` in WSL calls this script through powershell.exe. gramine-vbs.cmd next to
# this file runs it from cmd or PowerShell.
#
#   gramine-vbs check                              check the Windows side (VBS, Visual Studio, SDK, certificate)
#   gramine-vbs setup                              check Windows and WSL; install missing WSL tools user-level
#   gramine-vbs run    <DIR> [-Threads N] [-Root DIR] [-PidLine] [-ShowHost] [-- ARGS]
#   gramine-vbs sign   <DIR>
#   gramine-vbs verify <DIR> -Report FILE [-- verifier options]
#
# DIR is a package folder: the signed <name>.dll, pal_vbs_host.exe (and objects for `sign`); <name> is
# the folder's name without ".vbs", or -Name. Options for all commands: -Distro NAME (default
# $env:GRAMINE_VBS_DISTRO or Ubuntu-24.04), -CertName SUBJECT (default $env:GRAMINE_VBS_CERT_NAME or
# TheDefaultTestEnclaveCertName). The earlier form `run|sign|verify -Dir <folder> -Name <name>` still works.
# Arguments are parsed here (no param block): `--` must reach the script when it is started with
# powershell -File (gramine-vbs.cmd), where a param block would refuse it.
$Command = 'help'; $Target = ''; $Out = ''; $Distro = ''; $CertName = ''; $Threads = 0; $Root = ''
$Report = ''; $Dir = ''; $Name = ''; $ShowHost = $false; $PidLine = $false
$Rest = @()
$valueOpts = 'Out', 'Distro', 'CertName', 'Threads', 'Root', 'Report', 'Dir', 'Name'
$switchOpts = 'ShowHost', 'PidLine'
$pos = @()
for ($i = 0; $i -lt $args.Count; $i++) {
    $a = [string]$args[$i]
    if ($a -eq '--') { $Rest += @($args[($i + 1)..($args.Count)] | Where-Object { $_ -ne $null }); break }
    if ($a -match '^-(\w+)$') {
        $o = $valueOpts | Where-Object { $_ -eq $Matches[1] }
        if ($o) {
            if ($i + 1 -ge $args.Count) { Write-Host "gramine-vbs: -$o needs a value"; exit 2 }
            $i++; Set-Variable -Name $o -Value ([string]$args[$i]); continue
        }
        $o = $switchOpts | Where-Object { $_ -eq $Matches[1] }
        if ($o) { Set-Variable -Name $o -Value $true; continue }
        if ($pos.Count -ge 2) { $pos += $a; continue }   # an application argument
        Write-Host "gramine-vbs: unknown option $a"; exit 2
    }
    $pos += $a
}
if ($pos.Count -ge 1) { $Command = $pos[0] }
if ($pos.Count -ge 2) { $Target = $pos[1] }
if ($pos.Count -ge 3) { $Rest = @($pos[2..($pos.Count - 1)]) + $Rest }
$Threads = [int]$Threads
# the application's arguments of `run`, from gramine-vbs in WSL: JSON in base64 in the environment
# (no quoting layer in between), each one quoted for CommandLineToArgvW; Windows PowerShell 5.1
# passes a native command's arguments without escaping embedded quotes
if ($env:GRAMINE_VBS_ARGS) {
    $json = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($env:GRAMINE_VBS_ARGS))
    $Rest = @()
    $items = ConvertFrom-Json $json
    foreach ($x in $items) {
        $s = [string]$x -replace '(\\*)"', '$1$1\"'
        if ($s -eq '' -or $s -match '\s') { $s = '"' + ($s -replace '(\\+)$', '$1$1') + '"' }
        $Rest += $s
    }
}

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not $Distro) { $Distro = if ($env:GRAMINE_VBS_DISTRO) { $env:GRAMINE_VBS_DISTRO } else { 'Ubuntu-24.04' } }
if (-not $CertName) {
    $CertName = if ($env:GRAMINE_VBS_CERT_NAME) { $env:GRAMINE_VBS_CERT_NAME } else { 'TheDefaultTestEnclaveCertName' }
}

# a full path relative to PowerShell's location (not the process's current directory)
function Full-Path([string]$p) { return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p) }

# the architecture of this Windows machine and of the enclave images built for it: ARM64 or x64
# (dev docs: vbs-enclave/PLAN-x64.md); the WSL distribution must match it
$VbsArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'AMD64') { 'x64' } else { 'arm64' }
$LinuxArch = if ($VbsArch -eq 'x64') { 'x86_64' } else { 'aarch64' }

function Say($msg) { Write-Host "[gramine-vbs] $msg" }
function Fail($msg, $code = 1) { Write-Host "gramine-vbs: $msg"; exit $code }

# ---- Visual Studio, MSVC, the Windows SDK ----

function Find-Toolchain {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $t = @{ vs = $null; vc = $null; sdk = $null; sdkbin = $null }
    if (Test-Path $vswhere) { $t.vs = & $vswhere -latest -products * -property installationPath }
    if ($t.vs) {
        $t.vc = Get-ChildItem "$($t.vs)\VC\Tools\MSVC" -ErrorAction SilentlyContinue |
            Where-Object { Test-Path "$($_.FullName)\lib\$VbsArch\enclave\libcmt.lib" } |
            Sort-Object Name | Select-Object -Last 1
    }
    $t.sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path "$($_.FullName)\um\winenclave.h" } | Sort-Object Name | Select-Object -Last 1
    if ($t.sdk) { $t.sdkbin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\$($t.sdk.Name)\$VbsArch" }
    return $t
}

function Need-Toolchain {
    $t = Find-Toolchain
    if (-not $t.vs) { Fail 'no Visual Studio found (gramine-vbs setup)' }
    if (-not $t.vc) { Fail "no MSVC $VbsArch tools with the enclave CRT (gramine-vbs setup)" }
    if (-not $t.sdk) { Fail 'no Windows SDK with um\winenclave.h (gramine-vbs setup)' }
    return $t
}

# ---- WSL: every input travels in an environment variable (WSLENV), never on a command line ----

function Invoke-Wsl([string[]]$WslArgs, [hashtable]$Vars = @{}, [string[]]$PathVars = @()) {
    $saved = @{}
    $names = @()
    foreach ($k in $Vars.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k)
        [Environment]::SetEnvironmentVariable($k, [string]$Vars[$k])
        $names += $(if ($PathVars -contains $k) { "$k/p" } else { $k })
    }
    $oldWslenv = $env:WSLENV
    $env:WSLENV = (@($names) + @($(if ($oldWslenv) { $oldWslenv }))) -join ':'
    & wsl.exe -d $Distro @WslArgs | Out-Host    # (not into the function's return value)
    $rc = $LASTEXITCODE
    $env:WSLENV = $oldWslenv
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    return $rc
}

# Runs a shell script in the distribution: $Script is a Linux path, or a Windows path ($IsWinPath).
# The fixed command line below holds no quotes; IFS= and set -f keep the expanded path whole.
function Invoke-WslScript([string]$Script, [string]$Mode, [string]$Arg = '', [hashtable]$Vars = @{},
                          [string[]]$PathVars = @(), [switch]$IsWinPath) {
    $v = $Vars.Clone()
    $v['GV_SCRIPT'] = $Script
    $v['GV_MODE'] = $Mode
    $v['GV_ARG'] = $Arg
    $p = @($PathVars)
    if ($IsWinPath) { $p += 'GV_SCRIPT' }
    return Invoke-Wsl @('--cd', '~', '-e', 'sh', '-c', 'IFS=; set -f; exec sh $GV_SCRIPT $GV_MODE $GV_ARG') $v $p
}

function Wsl-Home {
    $h = & wsl.exe -d $Distro -e printenv HOME
    if ($LASTEXITCODE -ne 0 -or -not $h) { Fail "WSL distribution '$Distro' does not start (gramine-vbs setup)" }
    return ($h | Select-Object -Last 1).Trim()
}

# ---- the package folder ----

function Resolve-Build([string]$t, [switch]$MustExist) {
    if (-not $t) { Fail 'give a package folder' 2 }
    if (-not (Test-Path -PathType Container $t)) { Fail "no package folder $t" }
    $b = @{ out = (Convert-Path $t) }   # (Resolve-Path would prefix a \\wsl$ path with the provider name)
    $b.name = if ($Name) { $Name } else { (Split-Path -Leaf $b.out) -replace '\.vbs$', '' }
    if ($MustExist -and -not (Test-Path (Join-Path $b.out "$($b.name).dll"))) {
        Fail "$($b.out) holds no signed $($b.name).dll (run make VBS=1 in WSL)"
    }
    return $b
}

function Do-Sign($b, $t = $null) {
    if (-not $t) { $t = Need-Toolchain }
    if (-not (Test-Path (Join-Path $b.out 'pal_vbs_enclave.obj'))) { Fail "$($b.out) holds no objects (make VBS=1 in WSL)" }
    $t0 = Get-Date
    Push-Location $b.out
    try {
        $env:VBS_CERT_NAME = $CertName
        $env:VS_INSTALL_DIR = $t.vs
        cmd.exe /c "link-sign.cmd > link.log 2>&1"
        if ($LASTEXITCODE -ne 0 -or -not (Select-String -Quiet -Path link.log -Pattern 'build done')) {
            Get-Content link.log -Tail 25; Fail "link/sign failed ($($b.out)\link.log)"
        }
        Copy-Item pal_vbs_enclave.dll "$($b.name).dll" -Force
    } finally { Pop-Location }
    Say ("signed {0}\{1}.dll with the certificate {2} ({3:N1} s)" -f $b.out, $b.name, $CertName, ((Get-Date) - $t0).TotalSeconds)
}

function Do-Run($b) {
    $root = if ($Root) { Full-Path $Root } else { Join-Path $b.out 'root' }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    # the host folders the manifest allows (vbs.allowed_files entries ending in /), below the root
    $hd = Join-Path $b.out 'host-dirs.txt'
    if (Test-Path $hd) {
        Get-Content $hd | Where-Object { $_ } | ForEach-Object {
            New-Item -ItemType Directory -Force -Path (Join-Path $root $_) | Out-Null }
    }
    if ($PidLine) { Write-Output "[gramine-vbs-pid] $PID" }   # (gramine-vbs in WSL stops the host through it)
    # The host is started with one command-line string, so that the arguments reach it exactly as
    # quoted at the top (Windows PowerShell 5.1 re-quotes a native command's arguments). Its
    # standard output and error are printed as they arrive.
    $line = '"{0}.dll" {1} boot "{2}"' -f $b.name, $Threads, $root
    foreach ($x in $Rest) { $line += ' ' + $x }
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $b.out 'pal_vbs_host.exe'
    $psi.Arguments = $line
    $psi.WorkingDirectory = $b.out
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $proc = New-Object Diagnostics.Process
    $proc.StartInfo = $psi
    try { [void]$proc.Start() } catch { Fail "cannot start $($psi.FileName): $($_.Exception.Message)" }
    # Lines are read in order, on this thread (the OutputDataReceived events of .NET may be handled
    # out of order, which scrambled the output of programs that write many lines).
    $ot = $proc.StandardOutput.ReadLineAsync()
    $et = $proc.StandardError.ReadLineAsync()
    while ($ot -or $et) {
        $idle = $true
        if ($ot -and $ot.IsCompleted) {
            $l = $ot.Result; $idle = $false
            if ($null -eq $l) { $ot = $null } else {
                if ($ShowHost -or $l -notmatch '^\[(vbs-w|host)\]') { [Console]::Out.WriteLine($l) }
                $ot = $proc.StandardOutput.ReadLineAsync() }
        }
        if ($et -and $et.IsCompleted) {
            $l = $et.Result; $idle = $false
            if ($null -eq $l) { $et = $null } else {
                if ($ShowHost -or $l -notmatch '^\[(vbs-w|host)\]') { [Console]::Out.WriteLine($l) }
                $et = $proc.StandardError.ReadLineAsync() }
        }
        if ($idle) { Start-Sleep -Milliseconds 2 }
    }
    [Console]::Out.Flush()
    $proc.WaitForExit()
    $rc = $proc.ExitCode
    exit $rc
}

# ---- setup: check and install missing WSL packages ----

function Check-Item($ok, $what, $fix) {
    if ($ok) { Write-Host "  ok       $what" } else { Write-Host "  MISSING  $what"; Write-Host "           -> $fix"; $script:bad++ }
}

# The Windows side only; changes nothing. Returns after printing what is missing.
function Check-Windows {
    Write-Host 'Windows:'
    Check-Item ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITECTURE -eq 'AMD64') "ARM64 or x64 Windows ($env:PROCESSOR_ARCHITECTURE)" 'an ARM64 or x64 Windows 11 machine is needed'
    $dg = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue
    Check-Item ($dg -and $dg.VirtualizationBasedSecurityStatus -eq 2) 'VBS running' 'turn on memory integrity (Core isolation) and restart (Documentation/vbs.md)'
    $t = Find-Toolchain
    Check-Item $t.vs "Visual Studio ($($t.vs))" "install Visual Studio with Desktop development with C++ (and the $VbsArch build tools)"
    Check-Item $t.vc "MSVC $VbsArch with the enclave CRT ($($t.vc.Name))" "Visual Studio Installer: MSVC $VbsArch build tools"
    Check-Item $t.sdk "Windows SDK with winenclave.h ($($t.sdk.Name))" 'Visual Studio Installer: Windows 11 SDK 10.0.26100 or later'
    Check-Item ($t.sdkbin -and (Test-Path "$($t.sdkbin)\veiid.exe") -and (Test-Path "$($t.sdkbin)\signtool.exe")) "veiid.exe and signtool.exe (SDK, $VbsArch)" 'the Windows SDK signing tools'
    $cert = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq "CN=$CertName" }
    Check-Item $cert "signing certificate CN=$CertName (current user)" 'create one as Documentation/vbs.md describes, or give -CertName'
}

function Do-Check {
    $script:bad = 0
    Check-Windows
    if ($script:bad) { Write-Host "$($script:bad) item(s) missing"; exit 1 }
    Write-Host 'the Windows side is ready'
}

function Do-Setup {
    $script:bad = 0
    Check-Windows
    function Check($ok, $what, $fix) { Check-Item $ok $what $fix }
    Check (Get-Command tar.exe -ErrorAction SilentlyContinue) 'tar.exe' 'part of Windows 10 1803 and later'
    Write-Host 'WSL:'
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    Check $wsl 'wsl.exe' 'as administrator: wsl --install (needs a restart)'
    if (-not $wsl) { exit 1 }
    $env:WSL_UTF8 = '1'
    $list = (& wsl.exe -l -q) -replace "`0", '' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    $has = $list -contains $Distro
    Check $has "distribution $Distro" "wsl --install -d $Distro (or give -Distro / GRAMINE_VBS_DISTRO)"
    if (-not $has) { exit 1 }
    $arch = (& wsl.exe -d $Distro -e uname -m | Select-Object -Last 1)
    Check ($arch -eq $LinuxArch) "$Distro is $LinuxArch ($arch)" "a $LinuxArch distribution (Windows installs one of its own architecture by default)"
    $self = Join-Path ([IO.Path]::GetTempPath()) "gramine-vbs-setup-$PID.sh"
    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'wsl-setup.sh')) -replace "`r`n", "`n"
    [IO.File]::WriteAllText($self, $text, (New-Object Text.UTF8Encoding($false)))
    try {
        $rc = Invoke-WslScript $self 'user' 'clang' -IsWinPath
        $rc = Invoke-WslScript $self 'check' 'clang' -IsWinPath
        Check ($rc -eq 0) "build tools in $Distro" "gramine-vbs setup, or ./install.sh --vbs in WSL"
    } finally { Remove-Item -Force $self -ErrorAction SilentlyContinue }
    if ($script:bad) { Write-Host "$($script:bad) item(s) missing"; exit 1 }
    Write-Host 'everything needed is present'
}

# the earlier form: run|sign|verify -Dir <folder> -Name <name>
if ($Dir) {
    if (-not $Name) { Fail '-Dir needs -Name' 2 }
    $Target = $Dir
}

switch ($Command) {
    'check' { Do-Check }
    'setup' { Do-Setup }
    'sign' { Do-Sign (Resolve-Build $Target) }
    'run' {
        $b = Resolve-Build $Target
        $hostExe = Join-Path $b.out 'pal_vbs_host.exe'
        if (-not (Test-Path (Join-Path $b.out "$($b.name).dll")) -or -not (Test-Path $hostExe)) {
            Fail "no signed package at $($b.out); build it with make VBS=1 in WSL" 
        }
        Do-Run $b
    }
    'verify' {
        if (-not $Report) { Fail 'verify needs -Report <file>' 2 }
        $Report = Full-Path $Report
        $b = Resolve-Build $Target -MustExist
        Push-Location $b.out
        try {
            & .\pal_vbs_host.exe "$($b.name).dll" 1 verify $Report @Rest 2>&1 |
                Where-Object { $_ -notmatch '^\[host\]' } | ForEach-Object { "$_" }
            $rc = $LASTEXITCODE
        } finally { Pop-Location }
        exit $rc
    }
    default {
        Get-Content $PSCommandPath | Select-Object -Skip 3 -First 22 | ForEach-Object { $_ -replace '^# ?', '' }
        exit $(if ($Command -eq 'help') { 0 } else { 2 })
    }
}
