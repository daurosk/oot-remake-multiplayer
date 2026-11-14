# Job-started hook for the self-hosted runner (docs/ci-runner.md). install.ps1
# copies it with job-check.ps1 and runner-dirs.txt to C:\ProgramData\oot-ci\
# (admin-owned, read-only to the runner user) and points
# ACTIONS_RUNNER_HOOK_JOB_STARTED at it in the runner's .env and the machine
# environment. The runner runs it, as oot-ci, before any step of every job; a
# non-zero exit fails the job, so nothing from the job's checkout ever runs.
#
# It has no switches: nothing in the environment changes what it checks.
#  1. The job must be a push by daurosk to daurosk/oot-remake-multiplayer on main,
#     integrate-* or a tag (GITHUB_* come from GitHub's job message).
#  2. Every guarded folder (other profiles, every non-system top-level folder)
#     must be closed to oot-ci. A folder made after the install is open until
#     tools\ci-runner\deny.ps1 is run again, and until then no job runs.
$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'job-check.ps1')
    $names = 'GITHUB_EVENT_NAME', 'GITHUB_REPOSITORY', 'GITHUB_ACTOR', 'GITHUB_TRIGGERING_ACTOR', 'GITHUB_REF'
    $seen = @{}
    foreach ($n in $names) { $seen[$n] = [Environment]::GetEnvironmentVariable($n) }
    $line = ($names | ForEach-Object { "$_=$($seen[$_])" }) -join ' '
    if (-not (Test-OotCiJob -Env $seen)) {
        "oot-ci hook: REFUSED, this runner only takes pushes by daurosk to main, integrate-* or tags ($line)"
        exit 1
    }
    $skip = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'runner-dirs.txt')
    $open = @(Get-OotCiOpenFolders -Paths @(Get-OotCiGuardedFolders -Skip $skip))
    if ($open.Count) {
        "oot-ci hook: REFUSED, open to the runner user: $($open -join '; '). Run tools\ci-runner\deny.ps1 (elevated)."
        exit 1
    }
    "oot-ci hook: allowed ($line)"
    exit 0
} catch {
    "oot-ci hook: REFUSED, check failed: $_"
    exit 1
}
