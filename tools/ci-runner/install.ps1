# Installs the self-hosted GitHub Actions runner for daurosk/oot-remake-multiplayer
# on this PC (docs/ci-runner.md). Run ONCE in a fresh elevated PowerShell (Run as
# administrator, no transcript on), from a checkout of this repo:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\install.ps1
# Run tools\ci-runner\uninstall.ps1 first to install again.
#
# What it does:
# - standard local user oot-ci (Users only, never admin), random password nobody
#   sees; no interactive, RDP or network logon.
# - deny.ps1: Deny ACEs for oot-ci on every other profile and every non-system
#   top-level folder of a fixed drive (kits, games, toolchains); a no-create Deny
#   on each drive root and ProgramData. The list is saved so uninstall.ps1
#   removes exactly those. Folders made later stay open until deny.ps1 is run
#   again, and the hook refuses every job until then.
# - C:\actions-runner owned by Administrators: oot-ci may read and run it but
#   writes only _work and _diag. Registered with --disableupdate (re-run install
#   for a new runner release; GitHub refuses runners about 30 days out of date).
# - Job-started hook (job-started.ps1 + job-check.ps1) in C:\ProgramData\oot-ci,
#   read-only to oot-ci: refuses any job but a push by daurosk to main,
#   integrate-* or a tag, and any job while a guarded folder is open to oot-ci.
# - official runner zip, SHA-256 checked against GitHub's release notes; the
#   registration token is never printed.
param(
    [string]$Version = '2.338.2',
    [string]$Sha256 = 'a1f17aa987fc8158bc7521d284912267a0d01b7c2c4684b33c86783b5e8221cc',
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
if (Test-Path (Join-Path $Root 'config.cmd')) { throw "$Root already has a runner. Run tools\ci-runner\uninstall.ps1 first." }
$login = (& $Gh api user --jq .login)
if ($login -ne 'daurosk') { throw "gh is signed in as '$login', not daurosk." }

# 1. The user.
$bytes = New-Object byte[] 24
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$plain = [Convert]::ToBase64String($bytes) + 'a1!'
$secure = ConvertTo-SecureString $plain -AsPlainText -Force
# Windows limits: user name 20 characters, description 48 (job_started_test.ps1 checks both).
if (Get-LocalUser -Name $User -ErrorAction SilentlyContinue) {
    # Also fixes a user made by hand: its password must not expire under the service.
    Set-LocalUser -Name $User -Password $secure -PasswordNeverExpires $true -UserMayChangePassword $false -AccountNeverExpires
} else {
    New-LocalUser -Name $User -Password $secure -PasswordNeverExpires -UserMayNotChangePassword `
        -AccountNeverExpires -Description 'oot-remake-multiplayer CI runner' | Out-Null
}
Enable-LocalUser -Name $User
# Groups by SID (names are translated on non-English Windows). Membership is read
# through ADSI: Get-LocalGroupMember on Windows PowerShell 5.1 throws on groups
# that hold a deleted account or an Entra ID member.
Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $User -ErrorAction SilentlyContinue
$sid = (Get-LocalUser -Name $User).SID.Value
foreach ($g in 'S-1-5-32-544', 'S-1-5-32-555') {
    if (Test-OotCiInLocalGroup -GroupSid $g -MemberSid $sid) { throw "$User is in the group $g (Administrators or Remote Desktop Users); remove it first." }
}
$acct = "${env:COMPUTERNAME}\$User"

# 2. Hook folder and denied-folder list: admin-owned, oot-ci read only.
New-Item -ItemType Directory -Force $HookDir | Out-Null
& icacls $HookDir /setowner '*S-1-5-32-544' /T /C /Q | Out-Null
& icacls $HookDir /reset /T /C /Q | Out-Null
& icacls $HookDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' "${acct}:(OI)(CI)RX" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'icacls failed on the hook folder.' }
$hook = Join-Path $HookDir 'job-started.ps1'
foreach ($f in 'job-started.ps1', 'job-check.ps1') { Copy-Item (Join-Path $PSScriptRoot $f) (Join-Path $HookDir $f) -Force }
Set-Content -Encoding utf8 (Join-Path $HookDir 'runner-dirs.txt') $Root

# 3. Deny ACEs on everything of the owner's that oot-ci could reach.
& (Join-Path $PSScriptRoot 'deny.ps1') -Root $Root -HookDir $HookDir -User $User

# 4. Runner folder: admin-owned, oot-ci RX; download and verify the runner.
function Set-RunnerAcl {
    # Everything under the root inherits only from it; oot-ci writes only _work and _diag.
    foreach ($sub in '_work', '_diag') { New-Item -ItemType Directory -Force (Join-Path $Root $sub) | Out-Null }
    & icacls $Root /setowner '*S-1-5-32-544' /T /C /Q | Out-Null
    & icacls $Root /reset /T /C /Q | Out-Null
    & icacls $Root /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' "${acct}:(OI)(CI)RX" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'icacls failed on the runner folder.' }
    foreach ($sub in '_work', '_diag') {
        & icacls (Join-Path $Root $sub) /grant "${acct}:(OI)(CI)M" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "icacls failed on $sub." }
    }
}
New-Item -ItemType Directory -Force $Root | Out-Null
Set-RunnerAcl
$ProgressPreference = 'SilentlyContinue'  # the 5.1 progress bar makes a big download crawl
$zip =Join-Path $env:TEMP "actions-runner-win-x64-$Version.zip"
Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/actions/runner/releases/download/v$Version/actions-runner-win-x64-$Version.zip" -OutFile $zip
$got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($got -ne $Sha256) { Remove-Item $zip; throw "SHA-256 mismatch: got $got, expected $Sha256." }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($zip, $Root)
Remove-Item $zip
Set-Content -Encoding ascii (Join-Path $Root '.env') @(
    "ACTIONS_RUNNER_HOOK_JOB_STARTED=$hook",
    'PSExecutionPolicyPreference=RemoteSigned'
)
[Environment]::SetEnvironmentVariable('ACTIONS_RUNNER_HOOK_JOB_STARTED', $hook, 'Machine')

# 5. Register (token only in a variable) and install the service under oot-ci.
$token = (& $Gh api -X POST "repos/$Repo/actions/runners/registration-token" --jq .token)
if (-not $token) { throw 'No registration token.' }
Push-Location $Root
try {
    & .\config.cmd --unattended --url "https://github.com/$Repo" --token $token --name "$env:COMPUTERNAME-ci" `
        --labels daurosk-pc --work _work --replace --disableupdate --runasservice `
        --windowslogonaccount $acct --windowslogonpassword $plain
    if ($LASTEXITCODE -ne 0) { throw "config.cmd failed ($LASTEXITCODE)." }
} finally { Pop-Location; $token = $null; $plain = $null }

# 6. config.cmd grants the service account full control of the runner folder;
#    take that back: oot-ci reads and runs the runner, writes only _work and _diag.
#    The deny-logon rights go on only now: config.cmd checks the password with a
#    network logon, which "deny network logon" would make it reject.
$svc = Get-Service 'actions.runner.*' | Select-Object -First 1
Stop-Service $svc.Name
Set-OotCiLogonRights -Sid $sid -Add
Set-RunnerAcl
Start-Service $svc.Name
Start-Sleep -Seconds 5
if ((Get-Service $svc.Name).Status -ne 'Running') { throw "$($svc.Name) did not start; see $Root\_diag." }

# 7. Show the result (no secrets).
Get-Service 'actions.runner.*' | Format-Table Name, Status, StartType -AutoSize
& $Gh api "repos/$Repo/actions/runners" --jq '.runners[] | .name + " " + .status + " " + ([.labels[].name] | join(","))'
"Runner installed. Jobs use it only after: gh api -X POST repos/$Repo/actions/variables -f name=COOP_SELF_HOSTED -f value=on"
