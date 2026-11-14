# Removes everything install.ps1 set up (docs/ci-runner.md). Elevated PowerShell:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\uninstall.ps1
# Safe to run again after a half-finished install or uninstall. Every step runs
# even if an earlier one failed; the failures are listed at the end (exit 1).
# Only two things wait for a clean run: the hook folder (it holds denied.txt, the
# list of Deny ACEs to undo) stays while an ACE could not be removed, and the
# user stays while its profile could not be removed (both need the user's SID).
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

# Runs one step; a failure is recorded and the next step still runs.
$failures = New-Object System.Collections.Generic.List[string]
# $lastOk tells the caller whether the step just run worked.
function Run-Step([string]$Name, [scriptblock]$Body) {
    $script:lastOk = $true
    try { & $Body } catch { $script:lastOk = $false; $failures.Add("${Name}: $_"); "FAILED ${Name}: $_" }
}

# The account, by SID (install.ps1 saves it in user-sid.txt, so a run after the
# user is gone still finds its rights and profile).
$acctObj = Get-LocalUser -Name $User -ErrorAction SilentlyContinue
$sidFile = Join-Path $HookDir 'user-sid.txt'
$userSid = $null
if ($acctObj) { $userSid = $acctObj.SID.Value }
elseif (Test-Path $sidFile) { $userSid = ([string](Get-Content $sidFile -TotalCount 1)).Trim() }

# 1. Jobs go back to GitHub's runners first (a 404 when the variable is unset is fine).
Run-Step 'switch COOP_SELF_HOSTED off' {
    $ErrorActionPreference = 'Continue'
    & $Gh api -X PATCH "repos/$Repo/actions/variables/COOP_SELF_HOSTED" -f value=off 2>&1 | Out-Null
}

# 2. Unregister, then make sure the service is gone even if that failed.
if (Test-Path "$Root\config.cmd") {
    Run-Step 'unregister the runner' {
        $token = (& $Gh api -X POST "repos/$Repo/actions/runners/remove-token" --jq .token)
        if (-not $token) { throw 'no remove token from gh' }
        Push-Location $Root
        try { & .\config.cmd remove --token $token; $code = $LASTEXITCODE } finally { Pop-Location; $token = $null }
        if ($code -ne 0) { throw "config.cmd remove exited $code; delete the runner on GitHub by hand (Settings > Actions > Runners)" }
    }
}
Run-Step 'delete the runner service' {
    foreach ($svc in Get-Service 'actions.runner.*' -ErrorAction SilentlyContinue) {
        Stop-Service $svc.Name -Force -ErrorAction SilentlyContinue
        & sc.exe delete $svc.Name | Out-Null
        if ($LASTEXITCODE -ne 0 -and (Get-Service $svc.Name -ErrorAction SilentlyContinue)) { throw "sc.exe delete $($svc.Name) exited $LASTEXITCODE" }
    }
}

# 3. Undo the Deny ACEs, the logon rights and the hook setting.
$list = Join-Path $HookDir 'denied.txt'
$undenied = $true
if (Test-Path $list) {
    if ($userSid) {
        foreach ($d in Get-Content $list) {
            $p = ($d -split '\|')[0]
            if (-not $p -or -not (Test-Path $p)) { continue }
            Run-Step "undeny $p" {
                & icacls $p /remove:d "*$userSid" | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "icacls exited $LASTEXITCODE" }
                "undenied: $p"
            }
            if (-not $lastOk) { $undenied = $false }
        }
    } else {
        $undenied = $false
        $failures.Add("undeny: $list is there but the SID of $User is unknown (no user, no $sidFile); remove its Deny ACEs by hand")
    }
}
if ($userSid) { Run-Step 'remove the logon rights' { Set-OotCiLogonRights -Sid $userSid -Remove } }
Run-Step 'clear ACTIONS_RUNNER_HOOK_JOB_STARTED' {
    [Environment]::SetEnvironmentVariable('ACTIONS_RUNNER_HOOK_JOB_STARTED', $null, 'Machine')
}

# 4. Folders, profile, user.
Run-Step "remove $Root" { if (Test-Path $Root) { Remove-Item -Recurse -Force $Root } }
$profileGone = $true
if ($userSid) {
    Run-Step "remove the $User profile" {
        try {
            Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $userSid } | Remove-CimInstance
        } catch { throw "the profile is still loaded; reboot and run uninstall.ps1 again ($_)" }
    }
    $profileGone = $lastOk
}
if ($acctObj) {
    if ($profileGone) { Run-Step "remove the user $User" { Remove-LocalUser -SID $userSid } }
    else { "kept the user $User until its profile is removed." }
}
if ($undenied -and $profileGone) {
    Run-Step "remove $HookDir" { if (Test-Path $HookDir) { Remove-Item -Recurse -Force $HookDir } }
} else {
    "kept $HookDir (denied.txt, user-sid.txt) for the next run."
}

if ($failures.Count) {
    ''
    "Uninstall finished with $($failures.Count) failure(s); fix them and run uninstall.ps1 again:"
    foreach ($f in $failures) { "  - $f" }
    exit 1
}
"Runner, hook, Deny ACEs, logon rights, profile and user $User removed."
