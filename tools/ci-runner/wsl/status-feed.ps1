# Feeds the WSL runners' memory and disk guard (docs/ci-runner.md, "Linux runners in WSL"). install-wsl.ps1 copies
# it to %LOCALAPPDATA%\squadala-ci and starts it at every logon (scheduled task 'squadala-ci status feed', hidden).
# It runs `wsl.exe -d <distro> -u root -- /opt/squadala-ci/status-sink.sh` and writes
# "<free commit GB> <C: free GB> <lanes busy 0|1>" to its stdin every 15 s; job-started.sh waits while commit < 40 GB
# (lanes busy is only logged: jobs run while the lanes are busy, OWNER GO 2026-11-27) and refuses at C: < 60 GB. That wsl.exe also keeps the distro, and so the runners, running.
# If it exits it is started again after 15 s.
#
# Each tick it also sets the WSL VM process (vmmemWSL / vmmem) to BelowNormal priority, so the lane games win the CPU
# (Set-VmmemBelowNormal; failures are logged once, never fatal).
# Lanes busy (review-c123-14: free commit alone cannot see a game pair that is about to grow) comes from the test
# lanes' own state, never from process names: the lane locks <Root>\oot-remake-multiplayer\lane1.lock / lane2.lock
# (as offline-queue.ps1 LaneLockHeld reads them), and the first LANE1:/LANE2: lines of <Root>\coord-multiplayer\TEST-TURN
# (offline-queue.ps1 Holder): anything but empty / free... / (free... / none is busy, including 'offline-queue (job)'
# and an owner-playing line. An unreadable TEST-TURN counts as busy (fail closed). Only the 0/1 reaches the distro:
# TEST-TURN text never goes into a public job log.
param([string]$Distro = 'squadala-ci', [string]$Root = '')
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'status-feed.log'
function Write-FeedLog($m) { Add-Content -Encoding utf8 $log ("{0:u} {1}" -f (Get-Date).ToUniversalTime(), $m) }

function Test-LaneHolderFree([string]$Holder) {
    $h = "$Holder".Trim()
    return (-not $h) -or $h -like 'free*' -or $h -like '(free*' -or $h -eq 'none'
}
# 1 when a lane game or queue job may be running, else 0.
function Get-LanesBusy([string]$Root) {
    if (-not $Root) { return 1 }
    foreach ($n in 1, 2) { if (Test-Path -LiteralPath (Join-Path $Root "oot-remake-multiplayer\lane$n.lock")) { return 1 } }
    try { $tt = @(Get-Content -LiteralPath (Join-Path $Root 'coord-multiplayer\TEST-TURN') -Encoding UTF8 -ErrorAction Stop) } catch { return 1 }
    foreach ($n in 1, 2) {
        $l = $tt | ForEach-Object { $_.TrimStart([char]0xFEFF) } | Where-Object { $_ -match "^LANE${n}:" } | Select-Object -First 1
        if ($l -and -not (Test-LaneHolderFree ($l -replace "^LANE${n}:\s*", ''))) { return 1 }
    }
    return 0
}

$script:prioWarned = $false
# BelowNormal for the WSL VM (CI jobs) so the test-lane games keep the CPU first. Only ever lowers, never raises.
function Set-VmmemBelowNormal {
    foreach ($v in @(Get-Process -Name vmmemWSL, vmmem -ErrorAction SilentlyContinue)) {
        try {
            if ($v.PriorityClass -ne [Diagnostics.ProcessPriorityClass]::BelowNormal -and $v.PriorityClass -ne [Diagnostics.ProcessPriorityClass]::Idle) {
                $v.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal; Write-FeedLog "set $($v.Name) pid $($v.Id) to BelowNormal"
            }
        } catch { if (-not $script:prioWarned) { Write-FeedLog "cannot set $($v.Name) priority: $_"; $script:prioWarned = $true } }
    }
}

if ($MyInvocation.InvocationName -eq '.') { return }   # dot-sourced by the tests: functions only
while ($true) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:WINDIR 'System32\wsl.exe'
    $psi.Arguments = "-d $Distro -u root -- /opt/squadala-ci/status-sink.sh"
    $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.CreateNoWindow = $true
    try {
        $p = [Diagnostics.Process]::Start($psi)
        Write-FeedLog "started wsl.exe pid $($p.Id)"
        while (-not $p.HasExited) {
            $os = Get-CimInstance Win32_OperatingSystem
            $commit = [math]::Floor($os.FreeVirtualMemory / 1MB)
            $disk = [math]::Floor((New-Object IO.DriveInfo 'C').AvailableFreeSpace / 1GB)
            $p.StandardInput.WriteLine("$commit $disk $(Get-LanesBusy $Root)"); $p.StandardInput.Flush()
            Set-VmmemBelowNormal
            Start-Sleep -Seconds 15
        }
        Write-FeedLog "wsl.exe exited ($($p.ExitCode))"
    } catch { Write-FeedLog "error: $_" }
    Start-Sleep -Seconds 15
}
