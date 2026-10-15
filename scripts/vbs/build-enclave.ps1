# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# build-enclave.ps1: on one ARM64 Windows machine, from the pack.py output (made in WSL) and the
# sources to a signed enclave image, and optionally run it. The Windows enclave runtime and host are
# compiled with clang inside the WSL distribution (wsl-build.sh objs, build-objs.sh) against this
# machine's Windows SDK and MSVC headers; link /ENCLAVE, veiid and signtool run here
# (link-sign.cmd). Documentation/devel/vbs-enclave/PLAN-windows-build.md, phase W1.
#
#   build-enclave.ps1 -Repo <repo in WSL> -Pack <pack dir in WSL> -Out <Windows folder>
#                     [-Name hello] [-Distro Ubuntu-24.04] [-CertName <subject>]
#                     [-Run] [-SelfTest] [-Threads n]
#
# -Repo and -Pack are Linux paths inside the distribution (an empty -Pack builds an image without
# packed objects, the phase-1 image). -Out receives the objects, <Name>.dll (signed) and
# pal_vbs_host.exe. -Run runs the packed application (pal_vbs_host.exe <Name>.dll <threads> boot
# <Out>\root); -SelfTest runs the PAL-level self-test (pal_vbs_host.exe <Name>.dll <threads>).
# Inputs reach WSL as environment variables (WSLENV), never through a command line, so spaces and
# other characters in the paths need no quoting.
param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [string]$Pack = '',
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Name = 'enclave',
    [string]$Distro = 'Ubuntu-24.04',
    [string]$CertName = 'TheDefaultTestEnclaveCertName',
    [switch]$Run,
    [switch]$SelfTest,
    [int]$Threads = 1
)
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

function Fail($msg, $code = 1) { Write-Host "build-enclave: $msg"; exit $code }

# Visual Studio, MSVC and the Windows SDK (with the enclave header) of this machine
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail 'no Visual Studio (vswhere.exe not found)' }
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { Fail 'no Visual Studio installation found' }
$vc = Get-ChildItem "$vs\VC\Tools\MSVC" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
if (-not $vc -or -not (Test-Path "$($vc.FullName)\lib\arm64\enclave\libcmt.lib")) {
    Fail 'no MSVC ARM64 tools with the enclave CRT (VC\Tools\MSVC\<ver>\lib\arm64\enclave)'
}
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
$sdk = Get-ChildItem $kits -ErrorAction SilentlyContinue |
    Where-Object { Test-Path "$($_.FullName)\um\winenclave.h" } | Sort-Object Name | Select-Object -Last 1
if (-not $sdk) { Fail 'no Windows SDK with um\winenclave.h' }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# 1. objects in WSL
$env:GV_PACK = $Pack
$env:GV_OUT = $Out
$env:GV_HDRID = "$($sdk.Name)-$($vc.Name)"
$env:GV_HDRTAR = ''
$env:WSLENV = 'GV_PACK:GV_OUT/p:GV_HDRID:GV_HDRTAR/p'
$t0 = Get-Date
& wsl.exe -d $Distro --cd $Repo -e sh scripts/vbs/wsl-build.sh objs
$rc = $LASTEXITCODE
if ($rc -eq 10) {
    # the headers of this SDK and MSVC are not in the distribution yet: hand them over as one tar
    $tar = Join-Path $Out 'winhdr.tar'
    & tar.exe -cf $tar -C $sdk.FullName um shared ucrt
    if ($LASTEXITCODE -eq 0) { & tar.exe -rf $tar -C $vc.FullName include }
    if ($LASTEXITCODE -ne 0) { Fail 'tar.exe of the SDK and MSVC headers failed' }
    $env:GV_HDRTAR = $tar
    & wsl.exe -d $Distro --cd $Repo -e sh scripts/vbs/wsl-build.sh objs
    $rc = $LASTEXITCODE
    Remove-Item -Force $tar
}
if ($rc -ne 0) { Fail "compiling the objects in WSL ($Distro) failed (exit $rc)" }
$t1 = Get-Date
Write-Host ("objects: {0:N1} s" -f ($t1 - $t0).TotalSeconds)

# 2. link, veiid, sign (link-sign.cmd, copied next to the objects by wsl-build.sh)
Push-Location $Out
try {
    $env:VBS_CERT_NAME = $CertName
    $env:VS_INSTALL_DIR = $vs
    cmd.exe /c "link-sign.cmd > link.log 2>&1"
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -Quiet -Path link.log -Pattern 'build done')) {
        Get-Content link.log -Tail 25; Fail 'link/sign failed (link.log)'
    }
    Copy-Item pal_vbs_enclave.dll "$Name.dll" -Force
    $t2 = Get-Date
    Write-Host ("link + veiid + sign + host: {0:N1} s; signed {1}\{2}.dll (certificate {3})" -f `
        ($t2 - $t1).TotalSeconds, $Out, $Name, $CertName)

    # 3. run
    $rc = 0
    if ($SelfTest) {
        & .\pal_vbs_host.exe "$Name.dll" $Threads 2>&1 | ForEach-Object { "$_" }
        $rc = $LASTEXITCODE
    }
    if ($Run) {
        $root = Join-Path $Out 'root'
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        & .\pal_vbs_host.exe "$Name.dll" $Threads boot $root 2>&1 |
            Where-Object { $_ -notmatch '^\[(vbs-w|host)\]' } | ForEach-Object { "$_" }
        $rc = $LASTEXITCODE
    }
} finally {
    Pop-Location
}
exit $rc
