# Closes every guarded folder to the runner user oot-ci (docs/ci-runner.md).
# install.ps1 runs it; run it again (elevated) whenever the runner's hook refuses
# jobs with "open to the runner user", which it does for any folder made on a
# fixed drive or under Users after the last run:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\deny.ps1
# Each folder it denies is added to C:\ProgramData\oot-ci\denied.txt, so
# uninstall.ps1 removes exactly those Deny ACEs.
param(
    [string]$Root = 'C:\actions-runner',
    [string]$HookDir = 'C:\ProgramData\oot-ci',
    [string]$User = 'oot-ci'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rights.ps1')
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this in an elevated PowerShell (Run as administrator).' }
if (-not (Get-LocalUser -Name $User -ErrorAction SilentlyContinue)) { throw "No user $User; run install.ps1." }
if (-not (Test-Path $HookDir)) { throw "No $HookDir; run install.ps1." }

$acct = "${env:COMPUTERNAME}\$User"
$list = Join-Path $HookDir 'denied.txt'
$done = @(); if (Test-Path $list) { $done = @(Get-Content $list) }
$targets = @(Get-OotCiDenyTargets -Skip @($Root, $HookDir) -User $User)
# Save the list first, so an icacls failure part way still leaves every ACE
# that may have been set for uninstall.ps1 to remove.
@($done + $targets) | Select-Object -Unique | Set-Content -Encoding utf8 $list
foreach ($d in $targets) {
    $p, $kind = $d -split '\|'
    if ($kind -eq 'root') { & icacls $p /deny "${acct}:(AD,WD,WEA,WA)" | Out-Null }
    else { & icacls $p /deny "${acct}:(OI)(CI)F" | Out-Null }
    if ($LASTEXITCODE -ne 0) { throw "icacls deny failed on $p" }
    if ($done -notcontains $d) { "denied: $p ($kind)" }
}
"$($targets.Count) folders closed to $User."
