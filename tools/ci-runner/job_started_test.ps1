# Tests for job-started.ps1 (run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\job_started_test.ps1)
$ErrorActionPreference = 'Stop'
$hook = Join-Path $PSScriptRoot 'job-started.ps1'
$env:OOT_CI_HOOK_TEST = '1'
. $hook
$env:OOT_CI_HOOK_TEST = $null

$good = @{ GITHUB_EVENT_NAME = 'push'; GITHUB_REPOSITORY = 'daurosk/oot-remake-multiplayer'
           GITHUB_ACTOR = 'daurosk'; GITHUB_TRIGGERING_ACTOR = 'daurosk'; GITHUB_REF = 'refs/heads/main' }
$failed = 0
function Check($name, $change, $want) {
    $e = $good.Clone()
    foreach ($k in $change.Keys) { $e[$k] = $change[$k] }
    $got = Test-OotCiJob -Env $e
    if ($got -ne $want) { "FAIL $name (got $got)"; $script:failed++ } else { "ok   $name" }
}
Check 'push to main'            @{} $true
Check 'push to integrate-p13'   @{ GITHUB_REF = 'refs/heads/integrate-p13' } $true
Check 'tag'                     @{ GITHUB_REF = 'refs/tags/v0.10.1' } $true
Check 'pull_request'            @{ GITHUB_EVENT_NAME = 'pull_request'; GITHUB_REF = 'refs/pull/9/merge' } $false
Check 'pull_request_target'     @{ GITHUB_EVENT_NAME = 'pull_request_target' } $false
Check 'workflow_dispatch'       @{ GITHUB_EVENT_NAME = 'workflow_dispatch' } $false
Check 'fork repo'               @{ GITHUB_REPOSITORY = 'someone/oot-remake-multiplayer' } $false
Check 'other actor'             @{ GITHUB_ACTOR = 'someone' } $false
Check 'actor case'              @{ GITHUB_ACTOR = 'Daurosk' } $false
Check 'other re-runner'         @{ GITHUB_TRIGGERING_ACTOR = 'someone' } $false
Check 'work branch'             @{ GITHUB_REF = 'refs/heads/ci-runner' } $false
Check 'bare integrate-'         @{ GITHUB_REF = 'refs/heads/integrate-' } $false
Check 'missing values'          @{ GITHUB_ACTOR = $null; GITHUB_REF = $null } $false

# The real script, run the way the runner runs it, refuses an empty environment.
$names = 'GITHUB_EVENT_NAME', 'GITHUB_REPOSITORY', 'GITHUB_ACTOR', 'GITHUB_TRIGGERING_ACTOR', 'GITHUB_REF'
$saved = @{}; foreach ($n in $names) { $saved[$n] = [Environment]::GetEnvironmentVariable($n); [Environment]::SetEnvironmentVariable($n, $null) }
& powershell -NoProfile -ExecutionPolicy Bypass -command ". '$hook'" | Out-Null
$code = $LASTEXITCODE
foreach ($k in $good.Keys) { [Environment]::SetEnvironmentVariable($k, $good[$k]) }
& powershell -NoProfile -ExecutionPolicy Bypass -command ". '$hook'" | Out-Null
$codeGood = $LASTEXITCODE
foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
if ($code -ne 1) { "FAIL runner-style run, empty env exit $code"; $failed++ } else { 'ok   runner-style run refuses empty env' }
if ($codeGood -ne 0) { "FAIL runner-style run, good env exit $codeGood"; $failed++ } else { 'ok   runner-style run allows a daurosk push' }

if ($failed) { "$failed failed"; exit 1 }
'all job-started hook tests passed'
