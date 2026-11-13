# Installs the self-hosted GitHub Actions runner for daurosk/oot-remake-multiplayer
# on this PC (docs/ci-runner.md). Run ONCE in an elevated PowerShell (Run as
# administrator), from a checkout of this repo:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\install.ps1
# It creates the standard local user oot-ci (not an admin) with a random password
# nobody sees, a runner folder only that user, SYSTEM and Administrators can open,
# downloads the official runner, checks its SHA-256 against the one GitHub
# publishes in the release notes, registers it (the token never shows) and starts
# it as a service under oot-ci. Remove it all with tools\ci-runner\uninstall.ps1.
param(
    [string]$Version = '2.338.2',
    [string]$Sha256 = 'a1f17aa987fc8158bc7521d284912267a0d01b7c2c4684b33c86783b5e8221cc',
    [string]$Root = 'C:\actions-runner',
    [string]$User = 'oot-ci',
    [string]$Repo = 'daurosk/oot-remake-multiplayer',
    [string]$Gh = 'C:\Program Files\GitHub CLI\gh.exe'
)
$ErrorActionPreference = 'Stop'

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this in an elevated PowerShell (Run as administrator).' }
$login = (& $Gh api user --jq .login)
if ($login -ne 'daurosk') { throw "gh is signed in as '$login', not daurosk." }

# 1. The user: standard (Users group only), random 32-char password, never shown.
$bytes = New-Object byte[] 24
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$plain = [Convert]::ToBase64String($bytes) + 'a1!'
$secure = ConvertTo-SecureString $plain -AsPlainText -Force
if (Get-LocalUser -Name $User -ErrorAction SilentlyContinue) {
    Set-LocalUser -Name $User -Password $secure
} else {
    New-LocalUser -Name $User -Password $secure -PasswordNeverExpires -UserMayNotChangePassword `
        -AccountNeverExpires -Description 'GitHub Actions runner (oot-remake-multiplayer CI)' | Out-Null
}
Add-LocalGroupMember -Group 'Users' -Member $User -ErrorAction SilentlyContinue
if (Get-LocalGroupMember -Group 'Administrators' | Where-Object { $_.Name -like "*\$User" }) { throw "$User is an administrator; remove it from Administrators first." }

# 2. The folder: no inherited rights; SYSTEM + Administrators full, oot-ci modify.
New-Item -ItemType Directory -Force $Root | Out-Null
& icacls $Root /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' "${env:COMPUTERNAME}\${User}:(OI)(CI)M" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'icacls failed on the runner folder.' }

# 3. The owner's profile must stay closed to oot-ci (no Users/Everyone entries).
$open = (Get-Acl $env:USERPROFILE).Access | Where-Object {
    $_.IdentityReference -match ('Everyone|BUILTIN\\Users|Authenticated Users|' + [regex]::Escape($User)) }
if ($open) { throw "$env:USERPROFILE grants access to: $($open.IdentityReference -join ', '). Close it first." }

# 4. Download and verify the official runner.
$zip = Join-Path $Root "actions-runner-win-x64-$Version.zip"
Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/actions/runner/releases/download/v$Version/actions-runner-win-x64-$Version.zip" -OutFile $zip
$got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($got -ne $Sha256) { Remove-Item $zip; throw "SHA-256 mismatch: got $got, expected $Sha256." }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($zip, $Root)
Remove-Item $zip

# 5. Register (token only in a variable) and install the service under oot-ci.
$token = (& $Gh api -X POST "repos/$Repo/actions/runners/registration-token" --jq .token)
if (-not $token) { throw 'No registration token.' }
Push-Location $Root
try {
    & .\config.cmd --unattended --url "https://github.com/$Repo" --token $token --name "$env:COMPUTERNAME-ci" `
        --labels daurosk-pc --work _work --replace --runasservice `
        --windowslogonaccount ".\$User" --windowslogonpassword $plain
    if ($LASTEXITCODE -ne 0) { throw "config.cmd failed ($LASTEXITCODE)." }
} finally { Pop-Location; $token = $null; $plain = $null }

# 6. Show the result (no secrets).
Get-Service 'actions.runner.*' | Format-Table Name, Status, StartType -AutoSize
& $Gh api "repos/$Repo/actions/runners" --jq '.runners[] | .name + " " + .status + " " + ([.labels[].name] | join(","))'
"Runner installed. Jobs use it only after: gh api -X POST repos/$Repo/actions/variables -f name=COOP_SELF_HOSTED -f value=on"
