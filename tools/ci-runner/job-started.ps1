# Job-started hook for the self-hosted runner (docs/ci-runner.md). install.ps1
# copies it to C:\ProgramData\oot-ci\ (read-only to the runner user) and points
# ACTIONS_RUNNER_HOOK_JOB_STARTED at it in the runner's .env and the machine
# environment. The runner runs it before any step of every job; a non-zero exit
# fails the job, so nothing from the job's repo checkout ever runs.
#
# This is the real gate: a workflow file can be edited by whoever sends a pull
# request, this file cannot. A job is allowed only if it is a push by daurosk to
# daurosk/oot-remake-multiplayer on main, an integrate-* branch or a tag. The
# GITHUB_* values come from GitHub's job message; a workflow cannot override them.

function Test-OotCiJob {
    param([hashtable]$Env)
    $ref = [string]$Env['GITHUB_REF']
    return ([string]$Env['GITHUB_EVENT_NAME'] -ceq 'push') -and
        ([string]$Env['GITHUB_REPOSITORY'] -ceq 'daurosk/oot-remake-multiplayer') -and
        ([string]$Env['GITHUB_ACTOR'] -ceq 'daurosk') -and
        ([string]$Env['GITHUB_TRIGGERING_ACTOR'] -ceq 'daurosk') -and
        (($ref -ceq 'refs/heads/main') -or ($ref -clike 'refs/heads/integrate-?*') -or ($ref -clike 'refs/tags/?*'))
}

if ($MyInvocation.InvocationName -ne '.' -or $env:OOT_CI_HOOK_TEST -ne '1') {
    $names = 'GITHUB_EVENT_NAME', 'GITHUB_REPOSITORY', 'GITHUB_ACTOR', 'GITHUB_TRIGGERING_ACTOR', 'GITHUB_REF'
    $seen = @{}
    foreach ($n in $names) { $seen[$n] = [Environment]::GetEnvironmentVariable($n) }
    $line = ($names | ForEach-Object { "$_=$($seen[$_])" }) -join ' '
    if (Test-OotCiJob -Env $seen) {
        "oot-ci hook: allowed ($line)"
        exit 0
    }
    "oot-ci hook: REFUSED, this runner only takes pushes by daurosk to main, integrate-* or tags ($line)"
    exit 1
}
