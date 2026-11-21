# Tests for sending problem reports (tools/release/kit/scripts/problem-send.ps1): the question (No by default, no key in
# time is No), the answer kept with a random install id, a report queued only when the sessions had a problem or a crash
# record is new (never the same problem twice), the crash record masked, the gate before an upload, the queue (sent,
# tried again later, held), and one real upload to a listener on this PC (headers, the bot's answer). Made-up lines only.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\problem_send_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts'
. (Join-Path $scripts 'log-mask.ps1')
. (Join-Path $scripts 'problem-report.ps1')
. (Join-Path $scripts 'problem-send.ps1')
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Read-ZipText([string]$Zip) {
    $out = @{}
    $z = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($e in $z.Entries) {
            $r = New-Object System.IO.StreamReader($e.Open())
            try { $out[$e.FullName] = $r.ReadToEnd() } finally { $r.Dispose() }
        }
    } finally { $z.Dispose() }
    $out
}
function New-TestZip([string]$Path, [hashtable]$Files) {
    $stage = "$Path.stage"
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    foreach ($k in $Files.Keys) { [System.IO.File]::WriteAllText((Join-Path $stage $k), $Files[$k]) }
    Write-LogZip $stage $Path
    Remove-Item -LiteralPath $stage -Recurse -Force
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("problem-send-test-" + [guid]::NewGuid().ToString('N'))
$kit = Join-Path $tmp 'kit'
$game = Join-Path $tmp 'game'
New-Item -ItemType Directory -Path (Join-Path $kit 'logs'), (Join-Path $kit 'scripts'), (Join-Path $game 'bug-reports\pending') -Force | Out-Null
$env:OOT_COOP_PROBLEM_SEND_URL = ''
function New-Log([string]$Name, [string]$RunId, [string[]]$Lines) {
    $path = Join-Path $game $Name
    [System.IO.File]::WriteAllText($path, ((@("[hos host] run_id: $RunId") + $Lines) -join "`r`n"), (New-Object System.Text.UTF8Encoding $false))
    $path
}

# The question
function Key([string]$K) { $k2 = $K; { [pscustomobject]@{ Key = $k2 } }.GetNewClosure() }
Check ((Read-ProblemSendAnswer 5 (Key 'Y')) -eq $true) 'Y sends'
Check ((Read-ProblemSendAnswer 5 (Key 'N')) -eq $false) 'N does not'
Check ((Read-ProblemSendAnswer 5 (Key 'Enter')) -eq $false) 'Enter is No (the default)'
$t0 = Get-Date
Check ((Read-ProblemSendAnswer 1 { $null }) -eq $false) 'no key in time is No'
Check (((Get-Date) - $t0).TotalSeconds -lt 4) 'the question does not wait past its time'

# The address
Check ((Get-ProblemSendUrl $kit) -eq '') 'no address file: nothing to send to'
[System.IO.File]::WriteAllText((Join-Path $kit 'scripts\problem-send-url.txt'), "# comment`n`nhttps://reports.example.org/v1/problem-report`n")
Check ((Get-ProblemSendUrl $kit) -eq 'https://reports.example.org/v1/problem-report') 'the address is the first line that is not a comment'
[System.IO.File]::WriteAllText((Join-Path $kit 'scripts\problem-send-url.txt'), "http://reports.example.org/v1/problem-report`n")
Check ((Get-ProblemSendUrl $kit) -eq '') 'plain http to another PC is never used'
$env:OOT_COOP_PROBLEM_SEND_URL = 'http://127.0.0.1:1/v1/problem-report'
Check ((Get-ProblemSendUrl $kit) -eq 'http://127.0.0.1:1/v1/problem-report') 'http to this PC (tests) is'
$env:OOT_COOP_PROBLEM_SEND_URL = ''

# The answer kept
$s = Set-ProblemSendChoice $kit $false
Check ($s.send -eq 'off' -and -not $s.install) 'No: off, and no install id is made'
Check ((Read-ProblemState (Get-ProblemStateFile $kit)).choice -eq 'off') 'No also answers the older local question (one question)'
$s = Set-ProblemSendChoice $kit $true
Check ($s.send -eq 'on' -and $s.install -match '^[0-9a-f]{32}$') 'Yes: on, with a random install id'
$again = Read-ProblemSendState (Get-ProblemSendStateFile $kit)
Check ($again.send -eq 'on' -and $again.install -eq $s.install) 'the answer and the id are kept'
Check ((Read-ProblemState (Get-ProblemStateFile $kit)).choice -eq 'on') 'sending turns the reports on too'
$install = $s.install

# A quiet session: nothing
$quiet = New-Log 'longshot_log.1.txt' '11111111111111111111111111111111' @('[hos host] coop: host: player 2 joined: Mido', '[hos host] coop: scene: Lost Woods')
Check ($null -eq (Add-ProblemSendReport $kit 'kit: test' @($quiet) @($game) 'Testuser' 'TESTPC-01')) 'a quiet session sends nothing'
Check (@(Get-ChildItem (Get-ProblemSendDir $kit) -Filter *.zip -ErrorAction SilentlyContinue).Count -eq 0) 'and queues nothing'

# A session with problems and a crash record
$bad = New-Log 'longshot_log.2.txt' '22222222222222222222222222222222' @(
    '[hos host] coop: host: player 2 joined: Mido',
    '[hos Presentation Thread] coop: boss: could not move into the boss room; giving up',
    '[hos host] coop: link: lost the server 203.0.113.9:27510',
    '[hos host] coop: net: Mido timed out')
$crash = Join-Path $game 'bug-reports\pending\crash-20261121T180000Z.txt'
[System.IO.File]::WriteAllText($crash, "Exception 0xc0000005 in longshot.exe+0x1234`r`nPath: C:\Users\Testuser\Games\kit\longshot.exe`r`nPC: TESTPC-01`r`nserver 192.168.1.20 port 27510`r`nlast peer Mido; player 4 'Darunia' joined`r`nDarunia left`r`n")
[System.IO.File]::WriteAllText((Join-Path $game 'bug-reports\pending\unclean-20261121T170000Z.txt'), 'unclean')
$zip = Add-ProblemSendReport $kit 'kit: Squadala-v0.11.9-windows (release v0.11.9)' @($quiet, $bad) @($game) 'Testuser' 'TESTPC-01'
Check ($zip -and (Test-Path -LiteralPath $zip)) 'a session with problems queues a zip'
Check ((Split-Path -Leaf $zip) -match '^\d{8}-\d{6}-[0-9a-f]{32}\.zip$') 'named <date>-<report id>.zip'
$files = Read-ZipText $zip
Check ((@($files.Keys | Sort-Object) -join ',') -eq 'crash-record-1.txt,problem-report.txt') 'the report and the new crash record (not the unclean one)'
$pr = $files['problem-report.txt']
Check ($pr -match 'giving up' -and $pr -match '<name> timed out') 'the problem lines, names masked'
Check ($pr -notmatch '203\.0\.113' -and $pr -notmatch 'Mido') 'no address or name in them'
$cr = $files['crash-record-1.txt']
Check ($cr -match 'C:\\Users\\<user>\\Games' -and $cr -match 'PC: <pc>') 'the crash record: user and PC names masked'
Check ($cr -notmatch '192\.168' -and $cr -match '<lan-1>') 'the crash record: addresses masked'
Check ($cr -match 'longshot\.exe\+0x1234') 'the crash record: the crash itself kept'
Check ($cr -notmatch 'Mido|Darunia' -and $cr -match 'last peer <name>') 'the crash record: players'' names masked (from the logs and its own name line)'
Check (@(Get-ChildItem (Join-Path $kit 'logs') -Filter 'coop-report-*.zip').Count -eq 1) 'the local coop-report zip is made as before'
Check ((Test-ProblemSendZip $zip 'Testuser' 'TESTPC-01') -eq '') 'the queued zip passes the gate'

# Never the same problem twice
Check ($null -eq (Add-ProblemSendReport $kit '' @($quiet, $bad) @($game) 'Testuser' 'TESTPC-01')) 'the same sessions and crash: nothing new'
$bad2 = New-Log 'longshot_log.3.txt' '33333333333333333333333333333333' @('[hos Presentation Thread] coop: boss: could not move into the boss room; giving up')
Check ($null -eq (Add-ProblemSendReport $kit '' @($bad2) @($game) 'Testuser' 'TESTPC-01')) 'the same problem in a new session: nothing new'
[System.IO.File]::WriteAllText((Join-Path $game 'bug-reports\pending\crash-20261121T190000Z-1234-1.txt'), "Exception again near Mido`r`n")
$onlyCrash = Add-ProblemSendReport $kit 'kit: x' @($quiet, $bad) @($game) 'Testuser' 'TESTPC-01'
$f2 = Read-ZipText $onlyCrash
Check ($f2['crash-record-1.txt'] -match 'again near <name>') 'sessions read before: their names still mask a new crash record'
Check ($onlyCrash -and $f2['problem-report.txt'] -match '1 crash record' -and $f2['crash-record-1.txt'] -match 'again') 'a new crash alone is sent too'

# The gate
$g = Join-Path $tmp 'gate'
New-Item -ItemType Directory -Path $g -Force | Out-Null
$head = "Squadala! problem report: 1 new problem(s)`r`nKit: x`r`n`r`n"
New-TestZip "$g\ip.zip" @{ 'problem-report.txt' = $head + '1x coop: lost 203.0.113.9' }
Check ((Test-ProblemSendZip "$g\ip.zip" 'Testuser' 'TESTPC-01') -ne '') 'the gate stops an address'
New-TestZip "$g\user.zip" @{ 'problem-report.txt' = $head + '1x coop: could not write C:\Users\Testuser\x' }
Check ((Test-ProblemSendZip "$g\user.zip" 'Testuser' 'TESTPC-01') -ne '') 'the gate stops a user name'
New-TestZip "$g\pc.zip" @{ 'problem-report.txt' = $head + '1x coop: on TESTPC-01 failed' }
Check ((Test-ProblemSendZip "$g\pc.zip" 'Testuser' 'TESTPC-01') -ne '') 'the gate stops the PC name'
New-TestZip "$g\host.zip" @{ 'problem-report.txt' = $head + '1x coop: lost myhost.duckdns.org' }
Check ((Test-ProblemSendZip "$g\host.zip" 'Testuser' 'TESTPC-01') -ne '') 'the gate stops a host name'
New-TestZip "$g\code.zip" @{ 'problem-report.txt' = $head + '1x coop: failed, invite code: ABCDE-FGHJK-LMNPQ' }
Check ((Test-ProblemSendZip "$g\code.zip" 'Testuser' 'TESTPC-01') -ne '') 'the gate stops an invite code'
New-TestZip "$g\name.zip" @{ 'problem-report.txt' = $head; 'game-log.txt' = 'x' }
Check ((Test-ProblemSendZip "$g\name.zip" 'Testuser' 'TESTPC-01') -like 'file name*') 'the gate stops another file'
New-TestZip "$g\ok.zip" @{ 'problem-report.txt' = $head + '1x coop: lost <public-1>:27510, hookshot_coop.dll' }
Check ((Test-ProblemSendZip "$g\ok.zip" 'Testuser' 'TESTPC-01') -eq '') 'a masked report passes'

# The queue
$dir = Get-ProblemSendDir $kit
$calls = New-Object System.Collections.ArrayList
$retry = { param($z, $u, $i, $id) [void]$calls.Add($id); @{ Result = 'retry'; Why = 'no answer' } }.GetNewClosure()
$r = @(Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' $retry)
Check ($r.Count -eq 1 -and $r[0].Result -eq 'retry') 'no network: the first waits and the rest wait with it'
Check (@(Get-ChildItem $dir -Filter *.zip).Count -eq 2) 'both stay queued'
Check ((Get-Content (Join-Path $dir 'result.txt') -TotalCount 1) -match 'not sent yet') 'the result line says so'
$ids = @($zip, $onlyCrash | ForEach-Object { (Split-Path -Leaf $_) -replace '^.*-|\.zip$', '' })
Check ($ids -contains $calls[0]) 'the upload carries the report id'
$sent = { param($z, $u, $i, $id) [void]$calls.Add("$i"); @{ Result = 'sent'; Why = 'report #7' } }.GetNewClosure()
$r = @(Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' $sent)
Check ($r.Count -eq 2 -and ($r | Where-Object { $_.Result -ne 'sent' }).Count -eq 0) 'then both go'
Check ($calls[-1] -eq $install) 'with the install id'
Check (@(Get-ChildItem $dir -Filter *.zip).Count -eq 0 -and @(Get-ChildItem (Join-Path $dir 'sent') -Filter *.zip).Count -eq 2) 'a copy of each stays in sent\'
Check (-not (Test-Path (Join-Path $dir '*.tries'))) 'their tries are forgotten'
$sentList = @(Get-Content (Join-Path $game 'bug-reports\coop-sent.txt') -ErrorAction SilentlyContinue)
Check (($sentList -join ',') -in 'crash-20261121T180000Z.txt,crash-20261121T190000Z-1234-1.txt', 'crash-20261121T190000Z-1234-1.txt,crash-20261121T180000Z.txt') 'the sent crash records are listed for the game''s title (coop-sent.txt), by their own names'
Check (-not (Test-Path (Join-Path $dir '*.crashes'))) 'and their lists are gone'
Check ((Get-Content (Join-Path $dir 'result.txt') -TotalCount 1) -match 'sent to the Squadala! team \(report #7\)') 'the result line'
$r = @(Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' $sent)
Check ($r.Count -eq 0) 'nothing is sent twice'
Copy-Item "$g\ip.zip" (Join-Path $dir ('20261121-000000-' + ('a' * 32) + '.zip'))
$r = @(Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' { throw 'must not be called' })
Check ($r.Count -eq 1 -and $r[0].Result -eq 'held') 'a zip the gate stops is never sent'
Check (@(Get-ChildItem (Join-Path $dir 'held') -Filter *.zip).Count -eq 1) 'it is kept in held\'
Copy-Item "$g\ok.zip" (Join-Path $dir ('20261121-000001-' + ('b' * 32) + '.zip'))
$r = @(Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' { @{ Result = 'refused'; Why = 'answer 422' } })
Check ($r[0].Result -eq 'refused' -and @(Get-ChildItem (Join-Path $dir 'held') -Filter *.zip).Count -eq 2) 'a zip the bot refuses is held, not retried'
Copy-Item "$g\ok.zip" (Join-Path $dir ('20261121-000002-' + ('c' * 32) + '.zip'))
$script:SendMaxTries = 2
[void](Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' $retry)
$r = @(Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' $retry)
Check ($r[0].Result -eq 'gave up') 'one that never goes is given up after its tries (kept)'
$script:SendMaxTries = 30
Copy-Item "$g\ok.zip" (Join-Path $dir ('20261121-000003-' + ('d' * 32) + '.zip'))
[void](Set-ProblemSendChoice $kit $false)
$r = @(Send-ProblemQueue $kit 'https://reports.example.org/v1/problem-report' { throw 'must not be called' })
Check ($r.Count -eq 0) 'sending off: nothing goes'
[void](Set-ProblemSendChoice $kit $true)
$r = @(Send-ProblemQueue $kit '' { throw 'must not be called' })
Check ($r[0].Result -eq 'retry') 'no address: kept for later'

# One real upload (a listener on 127.0.0.1 answering as the bot does)
$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$ps = [powershell]::Create()
[void]$ps.AddScript({
    param($l)
    $c = $l.AcceptTcpClient()
    $s = $c.GetStream()
    $buf = New-Object byte[] 65536
    $got = New-Object System.IO.MemoryStream
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if ($s.DataAvailable) { $n = $s.Read($buf, 0, $buf.Length); $got.Write($buf, 0, $n) } else { Start-Sleep -Milliseconds 50 }
        $t = [System.Text.Encoding]::ASCII.GetString($got.ToArray())
        $i = $t.IndexOf("`r`n`r`n")
        if ($i -ge 0 -and $t -match 'Content-Length: (\d+)' -and $got.Length -ge $i + 4 + [int]$Matches[1]) { break }
    }
    $body = '{"ok": true, "report": 12}'
    $out = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body")
    $s.Write($out, 0, $out.Length)
    $c.Close()
    [System.Text.Encoding]::ASCII.GetString($got.ToArray())
}).AddArgument($listener)
$async = $ps.BeginInvoke()
$res = Send-ProblemZip "$g\ok.zip" "http://127.0.0.1:$port/v1/problem-report" $install ('e' * 32) 10000
$req = "$($ps.EndInvoke($async))"
$ps.Dispose(); $listener.Stop()
Check ($res.Result -eq 'sent' -and $res.Why -eq 'report #12') 'a real upload: the bot''s answer read'
Check ($req -match '^POST /v1/problem-report HTTP/1\.1') 'POST to the path'
Check ($req -match 'Content-Type: application/zip' -and $req -match "X-Squadala-Install: $install" -and $req -match "X-Squadala-Report: e{32}") 'the headers'
Check ($req -match 'PK\x03\x04') 'the zip itself'
$res = Send-ProblemZip "$g\ok.zip" "http://127.0.0.1:$port/v1/problem-report" $install ('e' * 32) 2000
Check ($res.Result -eq 'retry') 'nobody listening: tried again later'

Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all problem-send checks passed'
