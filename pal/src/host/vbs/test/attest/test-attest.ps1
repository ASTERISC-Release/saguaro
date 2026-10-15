# Phase-4 attestation test of the VBS-enclave PAL, run on Windows in the folder that holds
# pal_vbs_host.exe and the signed image of attest.c (pal_vbs_enclave.dll):
#
#   powershell -ExecutionPolicy Bypass -File test-attest.ps1 -Root <host root> [-OtherImage <dll>]
#
# 1. The application in the enclave writes user data to /dev/attestation/user_report_data and
#    saves /dev/attestation/report and /quote to <root>\data\out.
# 2. The host verifier (pal_vbs_host.exe <image> 1 verify) accepts them with the checks that a
#    relying party on this machine would make, and rejects every tampered or mismatching case.
# -OtherImage: a different signed image; verifying with it and --same-image must fail.
param([Parameter(Mandatory = $true)][string]$Root, [string]$OtherImage = '',
      [string]$Image = 'pal_vbs_enclave.dll')

# the host writes to stderr; PowerShell 5.1 would turn that into errors under 'Stop'
$ErrorActionPreference = 'Continue'
$out = Join-Path $Root 'data\out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$nonce = -join ((1..32) | ForEach-Object { '{0:x2}' -f (Get-Random -Maximum 256) })
$report = Join-Path $out 'report.bin'
Remove-Item $report -ErrorAction SilentlyContinue

$failed = 0
function Expect($name, $want, [string[]]$hostArgs) {
    $log = & .\pal_vbs_host.exe @hostArgs 2>&1
    $rc = $LASTEXITCODE
    $ok = ($rc -eq $want)
    if (-not $ok) { $script:failed++; $log | ForEach-Object { "    $_" } }
    '{0,-62} exit {1} (want {2}): {3}' -f $name, $rc, $want, $(if ($ok) { 'ok' } else { 'FAILED' })
}

Expect 'enclave application gets a report with user data' 0 @($Image, '0', 'boot', $Root, $nonce)
if (-not (Test-Path $report)) { 'no report written'; exit 1 }
$bytes = [IO.File]::ReadAllBytes($report)

function Variant($name, [scriptblock]$change) {
    $b = [byte[]]$bytes.Clone()
    & $change $b
    $p = Join-Path $out "$name.bin"
    [IO.File]::WriteAllBytes($p, $b)
    return $p
}

$v = @($Image, '1', 'verify')
Expect 'verify: user data, same image, svn >= 3' 0 ($v + @($report, '--user-data', $nonce, '--same-image', '--min-svn', '3'))
Expect 'verify: the quote (same package)' 0 ($v + @((Join-Path $out 'quote.bin'), '--user-data-file', (Join-Path $out 'user_data.bin'), '--same-image'))
Expect 'reject: one byte of the user data changed' 1 ($v + @((Variant 'tamper-data' { param($b) $b[24 + 8] = $b[24 + 8] -bxor 1 }), '--user-data', $nonce))
Expect 'reject: identity changed (svn byte)' 1 ($v + @((Variant 'tamper-svn' { param($b) $b[24 + 8 + 64 + 128] = $b[24 + 8 + 64 + 128] -bxor 1 })))
Expect 'reject: last signature byte changed' 1 ($v + @((Variant 'tamper-sig' { param($b) $b[$b.Length - 1] = $b[$b.Length - 1] -bxor 1 })))
Expect 'reject: package size field changed' 1 ($v + @((Variant 'bad-size' { param($b) $b[0] = $b[0] -bxor 4 })))
Expect 'reject: other user data expected' 1 ($v + @($report, '--user-data', ('ff' + $nonce.Substring(2))))
Expect 'reject: other unique id pinned' 1 ($v + @($report, '--unique-id', ('00' * 32)))
Expect 'reject: svn below the minimum' 1 ($v + @($report, '--min-svn', '4'))
if ($OtherImage) {
    Expect 'reject: verified by another image with --same-image' 1 @($OtherImage, '1', 'verify', $report, '--same-image')
    Expect 'accept: another image checks the signature only' 0 @($OtherImage, '1', 'verify', $report, '--user-data', $nonce)
}
if ($failed) { "attest test: $failed FAILED"; exit 1 }
'attest test: all cases passed'
