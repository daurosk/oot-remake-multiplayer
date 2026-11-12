# Checks a downloaded co-op kit zip (or Update.bat): that it is the file our release
# workflow signed, not only that it matches the .sha256 beside it (issue 161).
#
#   powershell -ExecutionPolicy Bypass -File tools\release\verify_kit.ps1 -Zip <oot-coop-....zip>
#
# 1. If <zip>.sha256 is beside it, the zip must match it.
# 2. The GitHub CLI (gh) checks the zip's artifact attestation: signed through Sigstore
#    by .github/workflows/release-attest.yml of this repository, for this exact file
#    (its SHA-256). No key is needed; gh fetches the attestation and checks the
#    signature, the certificate and the public transparency log entry.
#
# Exit codes: 0 checked and good; 1 the file does not check (do not use it);
# 2 could not check (no gh, no network). Release step (after the release is published
# and its "Release attest" run is green): run this on the local zip and Update.bat.

param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [string]$Repo = 'daurosk/oot-remake-multiplayer'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) {
    Write-Host "No such file: $Zip"
    exit 2
}
$Zip = (Resolve-Path -LiteralPath $Zip).Path
$hash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "File:    $Zip"
Write-Host "SHA-256: $hash"

$sumFile = "$Zip.sha256"
if (Test-Path -LiteralPath $sumFile -PathType Leaf) {
    $want = ([System.IO.File]::ReadAllText($sumFile).Trim() -split '\s+')[0].ToLowerInvariant()
    if ($want -ne $hash) {
        Write-Host "FAIL: the file does not match $([System.IO.Path]::GetFileName($sumFile)) ($want)."
        exit 1
    }
    Write-Host "OK: matches $([System.IO.Path]::GetFileName($sumFile))."
}

$gh = $null
$cmd = Get-Command gh -ErrorAction SilentlyContinue
if ($cmd) { $gh = $cmd.Source }
elseif (Test-Path -LiteralPath "$env:ProgramFiles\GitHub CLI\gh.exe") { $gh = "$env:ProgramFiles\GitHub CLI\gh.exe" }
if (-not $gh) {
    Write-Host 'Could not check the signature: the GitHub CLI (gh) is not installed.'
    Write-Host 'Get it from https://cli.github.com, run "gh auth login" once, then run this again.'
    exit 2
}

$workflow = "$Repo/.github/workflows/release-attest.yml"
Write-Host "Checking the attestation (signed by $workflow) ..."
$ErrorActionPreference = 'Continue'
& $gh attestation verify $Zip --repo $Repo --signer-workflow $workflow
$code = $LASTEXITCODE
if ($code -eq 0) {
    Write-Host 'OK: signed by our release workflow. This is the file the release published.'
    exit 0
}
Write-Host "FAIL: no valid attestation from $workflow for this file (gh exit $code)."
Write-Host 'Do not use it. (If gh said it could not reach GitHub or is not logged in, fix that and run this again.)'
exit 1
