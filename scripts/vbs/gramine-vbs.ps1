# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# gramine-vbs.ps1: the Windows side of gramine-vbs, without a Linux machine at hand. Works on the
# objects that `gramine-vbs build <manifest> --no-sign` prepared on Linux (the <name>.vbs\obj
# folder, copied to Windows), with link-sign.cmd and pal_vbs_enclave.def next to them.
#
#   gramine-vbs.ps1 sign   -Dir <folder> -Name <name> [-CertName <subject>]
#   gramine-vbs.ps1 run    -Dir <folder> -Name <name> [-Root <host root>] [-Threads n] [-- args]
#   gramine-vbs.ps1 verify -Dir <folder> -Name <name> -Report <file> [-- verifier options]
#
# sign: link, veiid and sign the enclave DLL (<name>.dll) and build pal_vbs_host.exe, with the
# certificate of the current user's store (default TheDefaultTestEnclaveCertName).
# run: run the application (pal_vbs_host.exe <name>.dll <threads> boot <root> args).
# verify: check an attestation report (pal_vbs_host.exe <name>.dll 1 verify <report> options).
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('sign', 'run', 'verify')][string]$Command,
    [string]$Dir = '.',
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$CertName = 'TheDefaultTestEnclaveCertName',
    [string]$Root = '',
    [int]$Threads = 0,
    [string]$Report = '',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest = @()
)
$ErrorActionPreference = 'Continue'
Set-Location $Dir
$Rest = @($Rest | Where-Object { $_ -ne '--' })

switch ($Command) {
    'sign' {
        if (-not (Test-Path 'link-sign.cmd') -or -not (Test-Path 'pal_vbs_enclave.obj')) {
            Write-Error "$Dir does not hold the prepared objects and link-sign.cmd"; exit 2
        }
        $env:VBS_CERT_NAME = $CertName
        cmd.exe /c "link-sign.cmd > link.log 2>&1"
        if ($LASTEXITCODE -ne 0 -or -not (Select-String -Quiet -Path link.log -Pattern 'build done')) {
            Get-Content link.log -Tail 25; exit 1
        }
        Copy-Item pal_vbs_enclave.dll "$Name.dll" -Force
        "signed: $(Resolve-Path "$Name.dll") (certificate $CertName)"
    }
    'run' {
        if (-not $Root) { $Root = Join-Path (Get-Location) 'root' }
        New-Item -ItemType Directory -Force -Path $Root | Out-Null
        & .\pal_vbs_host.exe "$Name.dll" $Threads boot $Root @Rest 2>&1 |
            Where-Object { $_ -notmatch '^\[(vbs-w|host)\]' } | ForEach-Object { "$_" }
        exit $LASTEXITCODE
    }
    'verify' {
        & .\pal_vbs_host.exe "$Name.dll" 1 verify $Report @Rest 2>&1 |
            Where-Object { $_ -notmatch '^\[host\]' } | ForEach-Object { "$_" }
        exit $LASTEXITCODE
    }
}
