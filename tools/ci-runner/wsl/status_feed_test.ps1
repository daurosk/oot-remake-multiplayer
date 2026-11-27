# Tests for status-feed.ps1's lane check (run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\wsl\status_feed_test.ps1)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'status-feed.ps1')
$failed = 0
function Ok($name, $cond) { if ($cond) { "ok   $name" } else { "FAIL $name"; $script:failed++ } }

$tmp = Join-Path $env:TEMP ("feed-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory (Join-Path $tmp 'coord-multiplayer'), (Join-Path $tmp 'oot-remake-multiplayer') | Out-Null
$tt = Join-Path $tmp 'coord-multiplayer\TEST-TURN'
function Turn([string[]]$lines) { [IO.File]::WriteAllText($tt, [char]0xFEFF + ($lines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding $false)) }
try {
    Turn 'LANE1: free', 'LANE2: free', '(an owner note)', 'OLD-LANE1: proof-0116 (old)'
    Ok 'both lanes free (BOM on line 1, OLD- lines ignored)' ((Get-LanesBusy $tmp) -eq 0)
    Turn 'LANE1: offline-queue (lane-prep-x-l1, unattended; started 19:55 UTC) [whole PC]', 'LANE2: free'
    Ok 'a queue job on lane 1 is busy' ((Get-LanesBusy $tmp) -eq 1)
    Turn 'LANE1: free', 'LANE2: rejoin-host-273 [WHOLE PC]'
    Ok 'a manual holder on lane 2 is busy' ((Get-LanesBusy $tmp) -eq 1)
    Turn 'LANE1: CLOSED, OWNER PLAYING', 'LANE2: free'
    Ok 'owner playing is busy' ((Get-LanesBusy $tmp) -eq 1)
    Turn 'LANE1: (free) done 00:3x UTC', 'LANE2: none'
    Ok '(free... and none are free' ((Get-LanesBusy $tmp) -eq 0)
    Turn 'LANE1: free', 'LANE2: free', 'LANE1: proof-0116-5 (a later line)'
    Ok 'only the first LANE1 line counts, as offline-queue.ps1 Holder' ((Get-LanesBusy $tmp) -eq 0)
    Turn 'LANE1: free', 'LANE2: free'
    New-Item -ItemType Directory (Join-Path $tmp 'oot-remake-multiplayer\lane2.lock') | Out-Null
    Ok 'a lane lock is busy even with TEST-TURN free' ((Get-LanesBusy $tmp) -eq 1)
    Remove-Item (Join-Path $tmp 'oot-remake-multiplayer\lane2.lock')
    Remove-Item $tt
    Ok 'no TEST-TURN is busy (fail closed)' ((Get-LanesBusy $tmp) -eq 1)
    Ok 'no root is busy (fail closed)' ((Get-LanesBusy '') -eq 1)
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
if ($failed) { "$failed failed"; exit 1 }
'all status feed tests passed'
