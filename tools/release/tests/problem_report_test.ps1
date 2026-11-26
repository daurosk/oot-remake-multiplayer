# Tests for the kit's problem reports (tools/release/kit/scripts/problem-report.ps1): which lines are problems, the
# masking (collect-logs' Protect-LogText) and the last gate for addresses, one fingerprint per problem, a problem in one
# report only (across sessions too), the zip's contents, and the answer kept. Made-up log lines only (no game output).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\problem_report_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts'
. (Join-Path $scripts 'log-mask.ps1')
. (Join-Path $scripts 'problem-report.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("problem-report-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
function New-Log([string]$Name, [string]$RunId, [string[]]$Lines) {
    $path = Join-Path $tmp $Name
    $text = (@("[hos host] run_id: $RunId") + $Lines) -join "`r`n"
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
    $path
}
$problems = @(
    '[hos sead::MainThread] coop: on, server 192.168.1.20:27510, name ''Saria'', join password set',
    '[hos sead::MainThread] coop: pose: Player_Child 309 bones, 0 moving; 0 sent/s, 0 received/s, 0 refused/s',
    '[hos Presentation Thread] coop: boss: could not move into the boss room of InsideTheDekuTree after 5 tries; giving up',
    '[hos Presentation Thread] coop: boss: could not move into the boss room of InsideTheDekuTree after 5 tries; giving up',
    '[hos host] coop: host: player 2 left: Mido (timed out: nothing heard for 10003 ms from 203.0.113.9:27510)',
    '[hos host] warning: coop: save: could not write C:\Users\Testuser\Games\coop\user\save\slot_01',
    '[hos host] coop: guard: a fault in co-op at 0x80021234 was caught (peer fe80::1c2b:3a4d:5e6f:7a8b%12)',
    '[hos host] coop: invite: could not copy: invite code: ABCDE-FGHJK-LMNPQ-RSTUV',
    '[hos host] coop: player 3 ''Ruto'' joined',
    '[hos host] coop: boss: could not hand Gohma to Saria or Ruto'
)

# A name in a problem line before the player's join line, and a name from another session read at the same time
$early = New-Log 'longshot_log.early.txt' '99999999999999999999999999999999' @('[hos host] coop: net: Nabooru timed out', '[hos host] coop: host: player 2 joined: Nabooru')
$earlyRun = Read-ProblemRun $early (New-MaskState) 'Testuser' 'TESTPC-01'
$earlyText = ($earlyRun.Events.Values | ForEach-Object { $_.Line }) -join "`n"
Check ($earlyText -match '<name> timed out' -and $earlyText -notmatch 'Nabooru') 'a name before its join line is masked too'
$shared = New-Object 'System.Collections.Generic.HashSet[string]'
$sa = New-Log 'longshot_log.sa.txt' '88888888888888888888888888888888' @('[hos host] coop: host: player 2 joined: Impa')
$sb = New-Log 'longshot_log.sb.txt' '77777777777777777777777777777777' @('[hos host] coop: link: lost Impa')
[void](Read-ProblemRun $sa (New-MaskState) 'Testuser' 'TESTPC-01' @() $shared)
$sbText = ((Read-ProblemRun $sb (New-MaskState) 'Testuser' 'TESTPC-01' @() $shared).Events.Values | ForEach-Object { $_.Line }) -join "`n"
Check ($shared.Contains('Impa') -and $sbText -match 'lost <name>' -and $sbText -notmatch 'Impa') 'names met in one session mask the next one read'
Remove-Item -LiteralPath $early, $sa, $sb -Force

# Which lines are problems
Check (Test-ProblemLine $problems[2]) 'a failure is a problem'
Check (-not (Test-ProblemLine $problems[1])) 'a routine counter (refused/s) is not a problem'
Check (-not (Test-ProblemLine '[hos host] coop: host: player 2 joined: Mido')) 'a join is not a problem'
Check (-not (Test-ProblemLine '[hos host] coop: scene: Lost Woods, entrance 3')) 'a scene named Lost Woods is not a problem'
Check (-not (Test-ProblemLine '[hos host] coop: guard: crash handler is on')) 'a note that mentions a crash is not a problem'
Check (-not (Test-ProblemLine '[hos host] coop: actors: unfailing wrongness')) 'the words are whole words'
Check (Test-ProblemLine '[hos host] coop: link: lost the server') 'lost, as the module writes it, is a problem'
Check (Test-ProblemLine '[hos host] CRASH: access violation at 0x1234') 'a CRASH line is a problem'

# One session: masked, one fingerprint per problem
$log1 = New-Log 'longshot_log.1.txt' '0123456789abcdef0123456789abcdef' $problems
$run = Read-ProblemRun $log1 (New-MaskState) 'Testuser' 'TESTPC-01'
Check ($run.Key -eq '0123456789abcdef0123456789abcdef') 'a session is known by its run_id'
$all = ($run.Events.Values | ForEach-Object { $_.Line }) -join "`n"
Check ($all -notmatch '192\.168|203\.0\.113|fe80|1c2b') 'no address is left in what is reported'
Check ($all -notmatch 'Testuser' -and $all -notmatch 'ABCDE-FGHJK') 'no user name or invite code is left'
Check ($all -match '<public-1>') 'an address becomes its kind (as collect-logs masks it)'
$boss = @($run.Events.Values | Where-Object { $_.Line -match 'giving up' })
Check ($boss.Count -eq 1 -and $boss[0].Count -eq 2) 'the same problem twice is one problem counted twice'
Check ((Get-ProblemFingerprint '[a] coop: x could not load slot 3 from <lan-1> after 12 ms') -eq
       (Get-ProblemFingerprint '[b] coop: x could not load slot 5 from <lan-2> after 40 ms')) 'a fingerprint ignores numbers and which address'

# Players' names: <name> where the module writes one, and wherever a known name comes up later
Check ($all -notmatch 'Mido|Saria|Ruto') 'no player name is left in what is reported'
Check ($all -match 'player 2 left: <name> \(timed out') 'a name becomes <name>'
Check ($all -match 'could not hand Gohma to <name> or <name>') 'a name seen in an earlier line is masked in a later one'
$n1 = New-Object 'System.Collections.Generic.HashSet[string]'
$n2 = New-Object 'System.Collections.Generic.HashSet[string]'
Check ((Get-ProblemFingerprint (Protect-ProblemNames 'coop: host: player 2 left: Mido (timed out)' $n1)) -eq
       (Get-ProblemFingerprint (Protect-ProblemNames 'coop: host: player 3 left: Darunia (timed out)' $n2))) 'another player with the same problem is the same fingerprint'

# The last gate: anything still an address or a host drops the line
Check (Test-ProblemHasAddress 'coop: could not reach 10.0.0.5') 'a raw address is caught by the gate'
Check (Test-ProblemHasAddress 'coop: could not reach 2001:db8::1') 'a raw IPv6 address is caught by the gate'
Check (Test-ProblemHasAddress 'coop: could not reach myhost.duckdns.org:27510') 'a host name with a port is caught by the gate'
Check (Test-ProblemHasAddress 'coop: could not reach myhost.duckdns.org') 'a host name is caught by the gate'
Check (Test-ProblemHasAddress 'coop: could not reach DESKTOP-AB12:27510') 'a bare host with a port is caught by the gate'
Check (-not (Test-ProblemHasAddress 'coop: could not reach <lan-1> (version 1.1.1, 10.0.26200.0)')) 'masks and versions pass the gate'
Check (-not (Test-ProblemHasAddress 'coop: could not reach <lan-1>:27510; hookshot_coop.dll not loaded from game.exe')) 'a masked address with a port and file names pass the gate'

# Reading: the key from the start only, a known session not read, a log another process is writing still read
Check ((Read-ProblemRun $log1 (New-MaskState) 'Testuser' 'TESTPC-01' @($run.Key)).Events -eq $null) 'a known session is not read past its start'
$plain = Join-Path $tmp 'hookshot_log.txt'
[System.IO.File]::WriteAllText($plain, ($problems -join "`r`n"), (New-Object System.Text.UTF8Encoding $false))
Check ((Read-ProblemRun $plain (New-MaskState) 'Testuser' 'TESTPC-01').Key -match '^file-\d+-[0-9a-f]{20}$') 'without a run_id a session is known by its size and start'
$writer = [System.IO.File]::Open($plain, 'Open', 'ReadWrite', 'Read')
try {
    $open = $null
    try { $open = Read-ProblemRun $plain (New-MaskState) 'Testuser' 'TESTPC-01' } catch { }
    Check ($null -ne $open -and $open.Events.Count -ge 5) 'a log another process has open for writing is still read'
} finally { $writer.Dispose() }
$locked = New-Log 'longshot_log.9.txt' '99990000999900009999000099990000' $problems
$lock = [System.IO.File]::Open($locked, 'Open', 'ReadWrite', 'None')
try {
    $skipState = @{ choice = 'on'; runs = @(); sent = [ordered]@{} }
    $r = $null
    try { $r = New-ProblemReport @($locked, $plain) $skipState (Join-Path $tmp 'skip') '' 'Testuser' 'TESTPC-01' } catch { }
    Check ($null -ne $r -and $skipState.runs.Count -eq 1) 'a log that cannot be opened is skipped, the others still read'
    Check ($skipState.runs -notcontains '99990000999900009999000099990000') 'the skipped log is tried again next time'
} finally { $lock.Dispose() }

# Reports: the new problems in a zip, each problem in one report only
$state = @{ choice = 'on'; runs = @(); sent = [ordered]@{} }
$first = New-ProblemReport @($log1) $state $tmp 'Squadala! 0.11.3' 'Testuser' 'TESTPC-01'
Check ($null -ne $first -and (Test-Path -LiteralPath $first.Zip)) 'the first report is a zip'
Check ($first.Count -ge 5) "the first report has the problems ($($first.Count))"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($first.Zip)
try {
    $entry = $zip.GetEntry('problem-report.txt')
    $reader = New-Object System.IO.StreamReader($entry.Open())
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
} finally { $zip.Dispose() }
Check ($text -match 'Send with /bugreport') 'the report says how to send it'
Check ($text -notmatch '192\.168|203\.0\.113|fe80|Testuser|ABCDE-FGHJK|Mido|Saria|Ruto') 'the zip holds no address, user name, code or player name'
Check ($null -eq (New-ProblemReport @($log1) $state $tmp '' 'Testuser' 'TESTPC-01')) 'the same session is never reported again'
$log2 = New-Log 'longshot_log.2.txt' 'fedcba9876543210fedcba9876543210' $problems
Check ($null -eq (New-ProblemReport @($log2) $state $tmp '' 'Testuser' 'TESTPC-01')) 'the same problems from another session are not reported again'
$log3 = New-Log 'longshot_log.3.txt' '11112222333344445555666677778888' @('[hos host] coop: flags: could not apply a new flag')
$third = New-ProblemReport @($log3) $state $tmp '' 'Testuser' 'TESTPC-01'
Check ($null -ne $third -and $third.Count -eq 1) 'a new problem makes a new report with only it'

# The answer and what was reported are kept
$file = Join-Path $tmp 'logs\problem-reports.json'
Write-ProblemState $file $state
$back = Read-ProblemState $file
Check ($back.choice -eq 'on') 'the answer is kept'
Check ($back.runs.Count -eq 3 -and $back.sent.Count -eq $state.sent.Count) 'the sessions read and the problems reported are kept'
Check ((Read-ProblemState (Join-Path $tmp 'none.json')).choice -eq '') 'never asked: no answer (the start asks once)'
$order = @{ choice = 'on'; runs = @(); sent = [ordered]@{} }
foreach ($i in 1..5003) { $order.sent[('fp{0:d5}' -f $i)] = 'd' }
Write-ProblemState $file $order
$kept = @((Read-ProblemState $file).sent.Keys)
Check ($kept.Count -eq 5000 -and $kept[0] -eq 'fp00004' -and $kept[-1] -eq 'fp05003') 'the oldest problems reported are the ones dropped'

# Only the newest report zips are kept
$zips = Join-Path $tmp 'zips'
New-Item -ItemType Directory -Path $zips -Force | Out-Null
foreach ($i in 1..12) { [System.IO.File]::WriteAllText((Join-Path $zips ('coop-report-2000010{0:d2}-000000.zip' -f $i)), 'x') }
$zipState = @{ choice = 'on'; runs = @(); sent = [ordered]@{} }
[void](New-ProblemReport @($log1) $zipState $zips '' 'Testuser' 'TESTPC-01')
$left = @(Get-ChildItem -LiteralPath $zips -Filter 'coop-report-*.zip' | Sort-Object Name)
Check ($left.Count -eq $script:ProblemZipsKept -and $left[0].Name -eq 'coop-report-200001004-000000.zip') "only the newest $($script:ProblemZipsKept) zips are kept"

Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'problem report tests: all passed'
