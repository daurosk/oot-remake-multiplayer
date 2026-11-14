# Tests for job-check.ps1 and job-started.ps1
# (run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\job_started_test.ps1)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'job-check.ps1')
$failed = 0
function Ok($name, $cond) { if ($cond) { "ok   $name" } else { "FAIL $name"; $script:failed++ } }

# --- Test-OotCiJob
$good = @{ GITHUB_EVENT_NAME = 'push'; GITHUB_REPOSITORY = 'daurosk/oot-remake-multiplayer'
           GITHUB_ACTOR = 'daurosk'; GITHUB_TRIGGERING_ACTOR = 'daurosk'; GITHUB_REF = 'refs/heads/main' }
function Job($change) { $e = $good.Clone(); foreach ($k in $change.Keys) { $e[$k] = $change[$k] }; Test-OotCiJob -Env $e }
Ok 'push to main'          (Job @{})
Ok 'push to integrate-p13' (Job @{ GITHUB_REF = 'refs/heads/integrate-p13' })
Ok 'tag'                   (Job @{ GITHUB_REF = 'refs/tags/v0.10.1' })
Ok 'pull_request refused'  (-not (Job @{ GITHUB_EVENT_NAME = 'pull_request'; GITHUB_REF = 'refs/pull/9/merge' }))
Ok 'pull_request_target refused' (-not (Job @{ GITHUB_EVENT_NAME = 'pull_request_target' }))
Ok 'workflow_dispatch refused'   (-not (Job @{ GITHUB_EVENT_NAME = 'workflow_dispatch' }))
Ok 'fork repo refused'     (-not (Job @{ GITHUB_REPOSITORY = 'someone/oot-remake-multiplayer' }))
Ok 'other actor refused'   (-not (Job @{ GITHUB_ACTOR = 'someone' }))
Ok 'actor case refused'    (-not (Job @{ GITHUB_ACTOR = 'Daurosk' }))
Ok 'other re-runner refused' (-not (Job @{ GITHUB_TRIGGERING_ACTOR = 'someone' }))
Ok 'work branch refused'   (-not (Job @{ GITHUB_REF = 'refs/heads/ci-runner' }))
Ok 'bare integrate- refused' (-not (Job @{ GITHUB_REF = 'refs/heads/integrate-' }))
Ok 'missing values refused'  (-not (Job @{ GITHUB_ACTOR = $null; GITHUB_REF = $null }))

# --- Get-OotCiOpenFolders, on made-up folders
$tmp = Join-Path $env:TEMP ("ootci-test-" + [guid]::NewGuid().ToString('N'))
$open = Join-Path $tmp 'open'; $empty = Join-Path $tmp 'empty'; $closed = Join-Path $tmp 'closed'
New-Item -ItemType Directory $open, $empty, $closed | Out-Null
Set-Content (Join-Path $open 'keys.txt') 'x'
$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
& icacls $closed /deny "${me}:(OI)(CI)(RD)" | Out-Null
try {
    $got = @(Get-OotCiOpenFolders -Paths @($open, $empty, $closed, (Join-Path $tmp 'missing')))
    Ok 'open folder found'         ($got -contains $open)
    Ok 'open empty folder found'   ($got -contains $empty)
    Ok 'denied folder not open'    ($got -notcontains $closed)
    Ok 'missing folder not open'   ($got.Count -eq 2)
} finally {
    & icacls $closed /remove:d $me | Out-Null
}

# --- Get-OotCiGuardedFolders
$all = @(Get-OotCiGuardedFolders)
$sysRoot = $env:SystemDrive + '\'
Ok 'system folders not guarded' (-not ($all | Where-Object { $OotCiSystemDirs -contains (Split-Path $_ -Leaf) -and (Split-Path $_ -Parent) -eq $sysRoot }))
Ok 'own profile not guarded'    (-not ($all | Where-Object { $_ -like '*\Users\oot-ci' }))
if ($all.Count) {
    $less = @(Get-OotCiGuardedFolders -Skip @($all[0]))
    Ok 'skip removes a folder' (($less -notcontains $all[0]) -and $less.Count -eq $all.Count - 1)
}

# --- install.ps1 against Windows limits (its first run stopped on a 49-character description)
$inst = Join-Path $PSScriptRoot 'install.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($inst, [ref]$null, [ref]$null)
$descs = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -in 'New-LocalUser', 'Set-LocalUser' }, $true) | ForEach-Object {
    $els = $_.CommandElements
    for ($i = 0; $i -lt $els.Count - 1; $i++) {
        if ($els[$i] -is [Management.Automation.Language.CommandParameterAst] -and $els[$i].ParameterName -eq 'Description') { $els[$i + 1] }
    }
})
Ok 'install.ps1 gives New-LocalUser a description' ($descs.Count -ge 1)
Ok 'every local user description is a constant of at most 48 characters' (-not ($descs | Where-Object {
    $_ -isnot [Management.Automation.Language.StringConstantExpressionAst] -or $_.Value.Length -gt 48 }))
$userDefault = ($ast.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq 'User' }).DefaultValue.Value
Ok 'runner user name is at most 20 characters' ($userDefault -and $userDefault.Length -le 20)
$src = Get-Content -Raw $inst
Ok 'deny-logon rights go on after config.cmd checks the password' ($src.IndexOf('Set-OotCiLogonRights -Sid $sid -Add') -gt $src.IndexOf('config.cmd --unattended'))
$scripts = 'install.ps1', 'deny.ps1', 'uninstall.ps1', 'rights.ps1' | ForEach-Object { Get-Content -Raw (Join-Path $PSScriptRoot $_) }
Ok 'no Get-LocalGroupMember (throws on 5.1 with orphaned members)' (-not ($scripts | Where-Object { $_ -match '(?m)^[^#]*Get-LocalGroupMember' }))

# --- Test-OotCiInLocalGroup (rights.ps1), on built-in members every PC has
. (Join-Path $PSScriptRoot 'rights.ps1')
Ok 'Authenticated Users is in Users'         (Test-OotCiInLocalGroup -GroupSid 'S-1-5-32-545' -MemberSid 'S-1-5-11')
Ok 'SYSTEM is not in Remote Desktop Users'   (-not (Test-OotCiInLocalGroup -GroupSid 'S-1-5-32-555' -MemberSid 'S-1-5-18'))

# --- The hook as the runner runs it (powershell -command ". 'hook'"), on a copy
#     laid out like C:\ProgramData\oot-ci. No variable changes what it checks.
$dep = Join-Path $tmp 'deployed'; New-Item -ItemType Directory $dep | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'job-started.ps1'), (Join-Path $PSScriptRoot 'job-check.ps1') $dep
Set-Content (Join-Path $dep 'runner-dirs.txt') 'C:\actions-runner'
$names = 'GITHUB_EVENT_NAME', 'GITHUB_REPOSITORY', 'GITHUB_ACTOR', 'GITHUB_TRIGGERING_ACTOR', 'GITHUB_REF', 'OOT_CI_HOOK_TEST'
$saved = @{}; foreach ($n in $names) { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
function RunHook($dir, $vars) {
    foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $null) }
    foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
    $hook = Join-Path $dir 'job-started.ps1'
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -command ". '$hook'" 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}
try {
    $r = RunHook $dep @{}
    Ok 'hook refuses an empty env'  ($r.Code -eq 1 -and $r.Out -match 'REFUSED')
    $bad = $good.Clone(); $bad['GITHUB_EVENT_NAME'] = 'pull_request'; $bad['GITHUB_ACTOR'] = 'someone'; $bad['OOT_CI_HOOK_TEST'] = '1'
    $r = RunHook $dep $bad
    Ok 'hook refuses a fork PR even with OOT_CI_HOOK_TEST=1' ($r.Code -eq 1 -and $r.Out -match 'REFUSED, this runner')
    $holes = @(Get-OotCiOpenFolders -Paths @(Get-OotCiGuardedFolders -Skip 'C:\actions-runner'))
    $r = RunHook $dep $good
    if ($holes.Count) { Ok "hook refuses a daurosk push while $($holes.Count) guarded folders are open" ($r.Code -eq 1 -and $r.Out -match 'open to the runner user') }
    else { Ok 'hook allows a daurosk push with every guarded folder closed' ($r.Code -eq 0 -and $r.Out -match 'allowed') }
    $broken = Join-Path $tmp 'broken'; New-Item -ItemType Directory $broken | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'job-started.ps1') $broken
    $r = RunHook $broken $good
    Ok 'hook without job-check.ps1 refuses (fails closed)' ($r.Code -eq 1 -and $r.Out -match 'check failed')
} finally {
    foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($failed) { "$failed failed"; exit 1 }
'all job-started hook tests passed'
