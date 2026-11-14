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
