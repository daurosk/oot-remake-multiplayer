# Removes everything install.ps1 set up (docs/ci-runner.md). Elevated PowerShell:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\uninstall.ps1
# Safe to run again after a half-finished install or uninstall.
param(
    [string]$Root = 'C:\actions-runner',
    [string]$HookDir = 'C:\ProgramData\oot-ci',
    [string]$User = 'oot-ci',
    [string]$Repo = 'daurosk/oot-remake-multiplayer',
    [string]$Gh = 'C:\Program Files\GitHub CLI\gh.exe'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rights.ps1')
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this in an elevated PowerShell (Run as administrator).' }

# 1. Jobs go back to GitHub's runners first (a 404 when the variable is unset is fine).
$ErrorActionPreference = 'Continue'
& $Gh api -X PATCH "repos/$Repo/actions/variables/COOP_SELF_HOSTED" -f value=off 2>&1 | Out-Null
$ErrorActionPreference = 'Stop'

# 2. Unregister, then make sure the service is gone even if that failed.
if (Test-Path "$Root\config.cmd") {
    $token = (& $Gh api -X POST "repos/$Repo/actions/runners/remove-token" --jq .token)
    Push-Location $Root
    try { & .\config.cmd remove --token $token; $removed = ($LASTEXITCODE -eq 0) } finally { Pop-Location; $token = $null }
    if (-not $removed) { "config.cmd remove failed; delete the runner on GitHub by hand (Settings > Actions > Runners)." }
}
foreach ($svc in Get-Service 'actions.runner.*' -ErrorAction SilentlyContinue) {
    Stop-Service $svc.Name -Force -ErrorAction SilentlyContinue
    & sc.exe delete $svc.Name | Out-Null
}

# 3. Undo the Deny ACEs, the logon rights and the hook setting.
$user = Get-LocalUser -Name $User -ErrorAction SilentlyContinue
$list = Join-Path $HookDir 'denied.txt'
if ($user -and (Test-Path $list)) {
    foreach ($d in Get-Content $list) {
        $p = ($d -split '\|')[0]
        if (Test-Path $p) { & icacls $p /remove:d "${env:COMPUTERNAME}\$User" | Out-Null; "undenied: $p" }
    }
}
if ($user) { Set-OotCiLogonRights -Sid $user.SID.Value -Remove }
[Environment]::SetEnvironmentVariable('ACTIONS_RUNNER_HOOK_JOB_STARTED', $null, 'Machine')

# 4. Folders, profile, user.
foreach ($p in $Root, $HookDir) { if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
if ($user) {
    Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $user.SID.Value } | Remove-CimInstance
    Remove-LocalUser -Name $User
}
"Runner, hook, Deny ACEs, logon rights, profile and user $User removed."
