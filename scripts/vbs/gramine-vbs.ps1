# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# gramine-vbs.ps1: gramine-vbs on one ARM64 or x64 Windows machine (Documentation/vbs.md). The Linux
# side of the build (the vbs glibc, the vbs PAL and LibOS, the application's vbs.build recipe,
# pack.py and the clang-compiled Windows objects) runs inside a WSL2 distribution of this machine;
# linking, veiid, signing and running happen here. gramine-vbs.cmd next to this file runs it.
#
#   gramine-vbs setup  [-Install | -Apt]              check the machine; -Install puts the build
#                                                     tools into the WSL distribution (no sudo),
#                                                     -Apt installs them with sudo apt-get
#   gramine-vbs build  <manifest> [-Out DIR] [-NoSign]
#   gramine-vbs sign   <manifest|DIR>
#   gramine-vbs run    <manifest|DIR> [-Threads N] [-Root DIR] [-Rebuild] [-ShowHost] [-- ARGS]
#                      (builds first when the image is missing or a source file changed)
#   gramine-vbs verify <manifest|DIR> -Report FILE [-- verifier options]
#
# Options for all commands: -Distro NAME (default $env:GRAMINE_VBS_DISTRO or Ubuntu-24.04),
# -CertName SUBJECT (default $env:GRAMINE_VBS_CERT_NAME or TheDefaultTestEnclaveCertName).
# The build folder DIR defaults to <manifest folder>\<name>.vbs. The repository this script is in
# is copied into the distribution's home (~/.cache/gramine-vbs/checkouts/...) and built there; a
# manifest outside the repository has its folder copied too (its vbs.packed_files must then stay
# inside that folder).
#
# For objects prepared elsewhere (gramine-vbs build --no-sign on Linux, copied here with
# link-sign.cmd and pal_vbs_enclave.def), the earlier form still works:
#   gramine-vbs sign|run|verify -Dir <folder> -Name <name> [...]
# Arguments are parsed here (no param block): `--` must reach the script when it is started with
# powershell -File (gramine-vbs.cmd), where a param block would refuse it.
$Command = 'help'; $Target = ''; $Out = ''; $Distro = ''; $CertName = ''; $Threads = 0; $Root = ''
$Report = ''; $Dir = ''; $Name = ''; $NoSign = $false; $Rebuild = $false; $ShowHost = $false
$Install = $false; $Apt = $false
$Rest = @()
$valueOpts = 'Out', 'Distro', 'CertName', 'Threads', 'Root', 'Report', 'Dir', 'Name'
$switchOpts = 'NoSign', 'Rebuild', 'ShowHost', 'Install', 'Apt'
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

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not $Distro) { $Distro = if ($env:GRAMINE_VBS_DISTRO) { $env:GRAMINE_VBS_DISTRO } else { 'Ubuntu-24.04' } }
if (-not $CertName) {
    $CertName = if ($env:GRAMINE_VBS_CERT_NAME) { $env:GRAMINE_VBS_CERT_NAME } else { 'TheDefaultTestEnclaveCertName' }
}
$RepoWin = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# a full path relative to PowerShell's location (not the process's current directory)
function Full-Path([string]$p) { return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p) }

# the architecture of this Windows machine and of the enclave images built for it: ARM64 or x64
# (Documentation/devel/vbs-enclave/PLAN-x64.md); the WSL distribution must match it
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

function Short-Hash([string]$s) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $b = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($s.ToLowerInvariant()))
    return (($b[0..3] | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Safe-Leaf([string]$p) { return ((Split-Path -Leaf $p) -replace '[^A-Za-z0-9._-]', '_') }

# Copies a Windows folder into the distribution (tar.exe here, wsl-build.sh sync there).
function Sync-Folder([string]$WinDir, [string]$Dest) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "gramine-vbs-$PID"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        $tar = Join-Path $tmp 'src.tar'
        & tar.exe -cf $tar --exclude .git --exclude '*.vbs' --exclude './build-vbs*' --exclude './build' `
            --exclude './subprojects/packagecache' -C $WinDir .
        if ($LASTEXITCODE -ne 0) { Fail "tar.exe of $WinDir failed" }
        # the sync step runs from an LF copy of wsl-build.sh (the checkout may have CRLF line ends)
        $self = Join-Path $tmp 'wsl-build.sh'
        $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'wsl-build.sh')) -replace "`r`n", "`n"
        [IO.File]::WriteAllText($self, $text, (New-Object Text.UTF8Encoding($false)))
        $rc = Invoke-WslScript $self 'sync' '' @{ GV_TAR = $tar; GV_DEST = $Dest } @('GV_TAR') -IsWinPath
        if ($rc -ne 0) { Fail "copying $WinDir into WSL failed (exit $rc)" }
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# ---- what a build was made from: names, sizes and times of the files that are copied to WSL ----

function Add-Stamp($sb, [string]$dir, [string]$rel) {
    foreach ($e in [IO.Directory]::EnumerateFileSystemEntries($dir)) {
        $n = [IO.Path]::GetFileName($e)
        $r = if ($rel) { "$rel/$n" } else { $n }
        if ([IO.Directory]::Exists($e)) {
            # the folders Sync-Folder leaves out
            if ($n -eq '.git' -or $n -like '*.vbs' -or (-not $rel -and ($n -like 'build-vbs*' -or $n -eq 'build')) -or
                $r -eq 'subprojects/packagecache') { continue }
            Add-Stamp $sb $e $r
        } else {
            $i = New-Object IO.FileInfo($e)
            [void]$sb.Append($r).Append('|').Append($i.Length).Append('|').Append($i.LastWriteTimeUtc.Ticks).Append("`n")
        }
    }
}

function Sources-Stamp($b) {
    $sb = New-Object Text.StringBuilder
    Add-Stamp $sb $RepoWin ''
    $mdir = Split-Path -Parent $b.manifest
    if (-not $b.manifest.StartsWith($RepoWin + '\', [StringComparison]::OrdinalIgnoreCase)) {
        [void]$sb.Append("== $mdir`n"); Add-Stamp $sb $mdir ''
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    return (($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($sb.ToString())) |
        ForEach-Object { $_.ToString('x2') }) -join '')
}

# ---- the build folder ----

function Resolve-Build([string]$t, [switch]$MustExist) {
    if (-not $t) { Fail "give a manifest or a build folder" }
    $b = @{}
    if (Test-Path -PathType Container $t) {
        $b.out = (Resolve-Path $t).Path
        $b.name = (Split-Path -Leaf $b.out) -replace '\.vbs$', ''
        $m = Join-Path $b.out 'manifest-path.txt'
        $b.manifest = if (Test-Path $m) { (Get-Content -Raw $m).Trim() } else { '' }
    } else {
        if (-not (Test-Path -PathType Leaf $t)) { Fail "no manifest $t" }
        $b.manifest = (Resolve-Path $t).Path
        $leaf = Split-Path -Leaf $b.manifest
        $b.name = $leaf -replace '\.manifest\.toml$', '' -replace '\.manifest\.vbs$', '' -replace '\.manifest$', ''
        $b.out = if ($Out) { Full-Path $Out } else {
            Join-Path (Split-Path -Parent $b.manifest) "$($b.name).vbs" }
    }
    if ($MustExist -and -not (Test-Path (Join-Path $b.out "$($b.name).dll"))) {
        Fail "$($b.out) holds no signed $($b.name).dll (gramine-vbs build)"
    }
    return $b
}

function Do-Build($b) {
    $t = Need-Toolchain
    $stamp = Sources-Stamp $b
    $home_ = Wsl-Home
    $mirror = "$home_/.cache/gramine-vbs/checkouts/$(Safe-Leaf $RepoWin)-$(Short-Hash $RepoWin)"
    $t0 = Get-Date
    Say "copy $RepoWin into WSL (${Distro}: $mirror)"
    Sync-Folder $RepoWin $mirror
    $mdir = Split-Path -Parent $b.manifest
    $rel = $null
    if ($b.manifest.StartsWith($RepoWin + '\', [StringComparison]::OrdinalIgnoreCase)) {
        $rel = $b.manifest.Substring($RepoWin.Length + 1) -replace '\\', '/'
        $mlinux = "$mirror/$rel"
    } else {
        $app = "$home_/.cache/gramine-vbs/apps/$(Safe-Leaf $mdir)-$(Short-Hash $mdir)"
        Say "copy $mdir into WSL ($app)"
        Sync-Folder $mdir $app
        $mlinux = "$app/$(Split-Path -Leaf $b.manifest)"
    }
    $t1 = Get-Date
    Say ("copied in {0:N1} s; Linux side in WSL" -f ($t1 - $t0).TotalSeconds)
    New-Item -ItemType Directory -Force -Path $b.out | Out-Null
    $vars = @{ GV_MANIFEST = $mlinux; GV_OUT = $b.out; GV_HDRID = "$($t.sdk.Name)-$($t.vc.Name)"; GV_HDRTAR = '' }
    $script = "$mirror/scripts/vbs/wsl-build.sh"
    $rc = Invoke-WslScript $script 'manifest' '' $vars @('GV_OUT', 'GV_HDRTAR')
    if ($rc -eq 10) {
        Say "first build with SDK $($t.sdk.Name) and MSVC $($t.vc.Name): handing their headers to WSL"
        $tar = Join-Path $b.out 'winhdr.tar'
        & tar.exe -cf $tar -C $t.sdk.FullName um shared ucrt
        if ($LASTEXITCODE -eq 0) { & tar.exe -rf $tar -C $t.vc.FullName include }
        if ($LASTEXITCODE -ne 0) { Fail 'tar.exe of the SDK and MSVC headers failed' }
        $vars.GV_HDRTAR = $tar
        $rc = Invoke-WslScript $script 'manifest' '' $vars @('GV_OUT', 'GV_HDRTAR')
        Remove-Item -Force $tar -ErrorAction SilentlyContinue
    }
    if ($rc -eq 11) { Fail "the WSL distribution lacks build tools: run 'gramine-vbs setup' (then -Install or -Apt)" }
    if ($rc -ne 0) { Fail "the Linux side of the build failed in WSL (exit $rc)" }
    Set-Content -Path (Join-Path $b.out 'manifest-path.txt') -Value $b.manifest -Encoding UTF8
    Set-Content -Path (Join-Path $b.out 'sources.stamp') -Value $stamp -Encoding ASCII
    Remove-Item -Force (Join-Path $b.out "$($b.name).dll") -ErrorAction SilentlyContinue
    Say ("Linux side done in {0:N1} s" -f ((Get-Date) - $t1).TotalSeconds)
    if (-not $NoSign) { Do-Sign $b $t }
}

function Do-Sign($b, $t = $null) {
    if (-not $t) { $t = Need-Toolchain }
    if (-not (Test-Path (Join-Path $b.out 'pal_vbs_enclave.obj'))) { Fail "$($b.out) holds no objects (gramine-vbs build)" }
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
    $hd = Join-Path $b.out 'host-dirs.txt'
    if (Test-Path $hd) {
        Get-Content $hd | Where-Object { $_ } | ForEach-Object {
            New-Item -ItemType Directory -Force -Path (Join-Path $root $_) | Out-Null }
    }
    Push-Location $b.out
    try {
        if ($ShowHost) {
            & .\pal_vbs_host.exe "$($b.name).dll" $Threads boot $root @Rest 2>&1 | ForEach-Object { "$_" }
        } else {
            & .\pal_vbs_host.exe "$($b.name).dll" $Threads boot $root @Rest 2>&1 |
                Where-Object { $_ -notmatch '^\[(vbs-w|host)\]' } | ForEach-Object { "$_" }
        }
        $rc = $LASTEXITCODE
    } finally { Pop-Location }
    exit $rc
}

# ---- setup: check, print what is missing, install into WSL only when asked ----

function Do-Setup {
    $script:bad = 0
    function Check($ok, $what, $fix) {
        if ($ok) { Write-Host "  ok       $what" } else { Write-Host "  MISSING  $what"; Write-Host "           -> $fix"; $script:bad++ }
    }
    Write-Host 'Windows:'
    Check ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITECTURE -eq 'AMD64') "ARM64 or x64 Windows ($env:PROCESSOR_ARCHITECTURE)" 'an ARM64 or x64 Windows 11 machine is needed'
    $dg = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue
    Check ($dg -and $dg.VirtualizationBasedSecurityStatus -eq 2) 'VBS running' 'turn on memory integrity (Core isolation) and restart (Documentation/vbs.md)'
    $t = Find-Toolchain
    Check $t.vs "Visual Studio ($($t.vs))" "install Visual Studio with Desktop development with C++ (and the $VbsArch build tools)"
    Check $t.vc "MSVC $VbsArch with the enclave CRT ($($t.vc.Name))" "Visual Studio Installer: MSVC $VbsArch build tools"
    Check $t.sdk "Windows SDK with winenclave.h ($($t.sdk.Name))" 'Visual Studio Installer: Windows 11 SDK 10.0.26100 or later'
    Check ($t.sdkbin -and (Test-Path "$($t.sdkbin)\veiid.exe") -and (Test-Path "$($t.sdkbin)\signtool.exe")) "veiid.exe and signtool.exe (SDK, $VbsArch)" 'the Windows SDK signing tools'
    $cert = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq "CN=$CertName" }
    Check $cert "signing certificate CN=$CertName (current user)" 'create one as Documentation/vbs.md describes, or give -CertName'
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
        if ($Install) {
            $rc = Invoke-WslScript $self 'user' 'clang' -IsWinPath
        } elseif ($Apt) {
            $pkgs = @()
            Invoke-WslScript $self 'missing' 'clang' -IsWinPath | Out-Null
            $pkgs = @((& wsl.exe -d $Distro -e cat /tmp/gramine-vbs-missing) -split ' ' | Where-Object { $_ })
            if ($pkgs.Count) {
                Write-Host "sudo apt-get install in ${Distro}: $($pkgs -join ' ')"
                & wsl.exe -d $Distro -e sudo apt-get update
                & wsl.exe -d $Distro -e sudo apt-get install -y --no-install-recommends @pkgs
            }
        }
        $rc = Invoke-WslScript $self 'check' 'clang' -IsWinPath
        Check ($rc -eq 0) "build tools in $Distro" "gramine-vbs setup -Install (no sudo: into ~/.local/share/gramine-vbs) or -Apt (sudo apt-get)"
    } finally { Remove-Item -Force $self -ErrorAction SilentlyContinue }
    if ($script:bad) { Write-Host "$($script:bad) item(s) missing"; exit 1 }
    Write-Host 'everything needed is present'
}

# ---- the earlier -Dir/-Name form ----

if ($Dir) {
    if (-not $Name) { Fail '-Dir needs -Name' }
    $b = @{ out = (Resolve-Path $Dir).Path; name = $Name; manifest = '' }
    switch ($Command) {
        'sign' { Do-Sign $b; exit 0 }
        'run' { Do-Run $b }
        'verify' { $Target = $b.out }
        default { Fail "-Dir works with sign, run and verify" }
    }
}

switch ($Command) {
    'setup' { Do-Setup }
    'build' {
        $b = Resolve-Build $Target
        if (-not $b.manifest) { Fail 'build needs a manifest' }
        Do-Build $b
    }
    'sign' { Do-Sign (Resolve-Build $Target) }
    'run' {
        $b = Resolve-Build $Target
        $dll = Join-Path $b.out "$($b.name).dll"
        # rebuild when asked, when unsigned, or when a file of the checkout (or of the manifest's
        # folder, if it is outside) changed since the build: names, sizes and times
        $old = Join-Path $b.out 'sources.stamp'
        if ($b.manifest -and ($Rebuild -or -not (Test-Path $dll) -or -not (Test-Path $old) -or
                (Get-Content -Raw $old).Trim() -ne (Sources-Stamp $b))) {
            $NoSign = $false
            Do-Build $b
        }
        if (-not (Test-Path $dll)) { Fail "$dll does not exist (gramine-vbs build)" }
        Do-Run $b
    }
    'verify' {
        if (-not $Report) { Fail 'verify needs -Report <file>' }
        $Report = Full-Path $Report
        $b = if ($Dir) { @{ out = (Resolve-Path $Dir).Path; name = $Name } } else { Resolve-Build $Target -MustExist }
        Push-Location $b.out
        try {
            & .\pal_vbs_host.exe "$($b.name).dll" 1 verify $Report @Rest 2>&1 |
                Where-Object { $_ -notmatch '^\[host\]' } | ForEach-Object { "$_" }
            $rc = $LASTEXITCODE
        } finally { Pop-Location }
        exit $rc
    }
    default {
        Get-Content $PSCommandPath | Select-Object -Skip 3 -First 25 | ForEach-Object { $_ -replace '^# ?', '' }
        exit $(if ($Command -eq 'help') { 0 } else { 2 })
    }
}
