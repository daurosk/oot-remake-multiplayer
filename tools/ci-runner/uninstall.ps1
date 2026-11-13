# Removes the self-hosted runner installed by install.ps1 (docs/ci-runner.md).
# Elevated PowerShell:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\uninstall.ps1
param(
    [string]$Root = 'C:\actions-runner',
    [string]$User = 'oot-ci',
    [string]$Repo = 'daurosk/oot-remake-multiplayer',
    [string]$Gh = 'C:\Program Files\GitHub CLI\gh.exe'
)
$ErrorActionPreference = 'Stop'

# Jobs go back to GitHub's runners first.
& $Gh api -X DELETE "repos/$Repo/actions/variables/COOP_SELF_HOSTED" 2>$null
if (Test-Path "$Root\config.cmd") {
    $token = (& $Gh api -X POST "repos/$Repo/actions/runners/remove-token" --jq .token)
    Push-Location $Root
    try { & .\config.cmd remove --token $token } finally { Pop-Location; $token = $null }
}
if (Test-Path $Root) { Remove-Item -Recurse -Force $Root }
if (Get-LocalUser -Name $User -ErrorAction SilentlyContinue) { Remove-LocalUser -Name $User }
"Runner, folder and user $User removed."
