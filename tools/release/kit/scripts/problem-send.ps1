# Sending problem reports (docs/problem-reports.md, "Sending"): after a game closes, the masked problem report of the
# finished sessions (problem-report.ps1: the co-op problem lines and crash lines) and Longshot's new crash records,
# masked the same way, go to the Squadala! report bot, which files them for the team. Opt-in, asked at a start with No
# as the default, and only when the sessions had a problem: a quiet session sends nothing. Nothing goes to Longshot.
#
#   -AtStart       start.ps1, step 5b: asks once (No in 20 s), says the last result, then what problem-report.ps1
#                  -AtStart does (with sending on: the new problems are queued) and sends the queue in a hidden window.
#   -Watch <pid>   play.ps1, right after the game started: with sending on, a hidden window waits for that game to
#                  close (a crash too), then makes the report and sends it. Returns at once.
#   -AfterGame     that hidden window (-GamePid, -GameStart).
#   -SendPending   send what is queued (a hidden window from -AtStart).
#   on | off       problem-report.cmd on / off: off turns sending off too; send turns sending (and reports) on.
#
# The queue: logs\problem-send\<id>.zip, each zip sent once (the bot answers a resend 'already received'); a sent zip is
# kept in logs\problem-send\sent\ (the newest 10), and the last result is one line in logs\problem-send\result.txt.
# Before each upload the zip is checked again (Test-ProblemSendZip): only the kit's own file names, small, and every line
# as the masking leaves it (masking it again changes nothing, and no address, host, user or PC name is left in it). A zip
# that fails stays here and is never sent.
# Dot-source it for its functions (tools\release\tests\problem_send_test.ps1).
param([Parameter(Position = 0)][ValidateSet('on', 'off', 'send', 'nosend')][string]$Turn, [switch]$AtStart,
      [int]$Watch = 0, [switch]$AfterGame, [int]$GamePid = 0, [long]$GameStart = 0, [switch]$SendPending)

# How long a start waits for the answer (no key: no)
$script:SendAskSeconds = 20
# An upload's whole time (connect, send, answer)
$script:SendTimeoutMs = 15000
# A zip not sent after this many tries (each start and each game's end tries once) or days is given up (kept locally)
$script:SendMaxTries = 30
$script:SendMaxDays = 30
$script:SendKept = 10
$script:SendNames = '^(?:problem-report\.txt|crash-record-[1-3]\.txt)$'
$script:SendMaxCrashBytes = 64 * 1024

function Get-ProblemSendDir([string]$Kit) { Join-Path $Kit 'logs\problem-send' }
function Get-ProblemSendStateFile([string]$Kit) { Join-Path $Kit 'logs\problem-send.json' }

# The address the kit sends to: OOT_COOP_PROBLEM_SEND_URL (tests), else scripts\problem-send-url.txt (its first line
# that is not a # comment; the release puts the bot's address there). HTTPS only (http only to this PC, for tests); ''
# when there is none: then the start does not ask and nothing is sent.
function Get-ProblemSendUrl([string]$Kit) {
    $u = "$env:OOT_COOP_PROBLEM_SEND_URL".Trim()
    if (-not $u) {
        try {
            $u = "$(@(Get-Content -LiteralPath (Join-Path $Kit 'scripts\problem-send-url.txt') -TotalCount 20 -ErrorAction Stop |
                Where-Object { $_.Trim() -and $_.Trim() -notmatch '^#' }) | Select-Object -First 1)".Trim()
        } catch { }
    }
    if ($u -match '^https://[A-Za-z0-9.-]+(:\d+)?/[\w./-]*$') { return $u }
    if ($u -match '^http://(127\.0\.0\.1|localhost)(:\d+)?/[\w./-]*$') { return $u }
    ''
}

# The answer ('' = never asked, on, off), this install's random id (made when the player says yes; 32 hex digits,
# nothing about the PC or the player), and the crash records already in a report.
function Read-ProblemSendState([string]$Path) {
    $s = @{ send = ''; install = ''; crashes = @(); asked = '' }
    try {
        $j = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($j.send -in @('on', 'off')) { $s.send = $j.send }
        if ("$($j.install)" -match '^[0-9a-f]{32}$') { $s.install = "$($j.install)" }
        if ($j.crashes) { $s.crashes = @($j.crashes | ForEach-Object { "$_" }) }
        if ($j.asked) { $s.asked = "$($j.asked)" }
    } catch { }
    $s
}

function Write-ProblemSendState([string]$Path, [hashtable]$State) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $json = [ordered]@{ send = $State.send; install = $State.install; asked = $State.asked
        crashes = @($State.crashes | Select-Object -Last 200) } | ConvertTo-Json -Depth 4
    $tmp = "$Path.new"
    [System.IO.File]::WriteAllText($tmp, $json, (New-Object System.Text.UTF8Encoding $false))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

function New-ProblemSendId { [guid]::NewGuid().ToString('N') }

# Two locks (a game's end and a start, or two games, at the same moment): 'reports' for the two state files while a
# report is made, 'queue' while the queue is sent (an upload can take seconds; a start never waits for it). $null when
# another holds it longer than $Seconds.
function Enter-ProblemSendLock([int]$Seconds = 60, [string]$Which = 'reports') {
    $m = New-Object System.Threading.Mutex($false, "Local\SquadalaProblem-$Which")
    try { if ($m.WaitOne($Seconds * 1000)) { return $m } } catch [System.Threading.AbandonedMutexException] { return $m }
    $m.Dispose()
    $null
}
function Exit-ProblemSendLock($Lock) { if ($Lock) { try { $Lock.ReleaseMutex() } catch { }; $Lock.Dispose() } }

# The question (owner, 0.11.9): Y is yes; N, Enter or no key in $Seconds is no; no console is no. $ReadKey: tests.
function Read-ProblemSendAnswer([int]$Seconds = $script:SendAskSeconds, [scriptblock]$ReadKey = $null, [string]$Url = '') {
    $q = 'Send problem reports to the Squadala! team after your games? [y/N]'
    Write-Host ''
    Write-Host 'Problem reports: when a game you played had a co-op problem or a crash, the kit can send the team a small'
    Write-Host '  report after the game closes: the problem lines from the game''s log and the crash record, masked first'
    Write-Host '  (IP and MAC addresses, your user and PC names, players'' names, invite codes and passwords are taken'
    Write-Host '  out; lines that may still hold an address are left out). Nothing is sent after a game without problems.'
    Write-Host "  It goes to the Squadala! report bot$(if ($Url) { " ($(([uri]$Url).Host))" }), never to Longshot; a copy stays in logs\problem-send."
    Write-Host '  Turn it off at any time: scripts\problem-report.cmd off.'
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected } catch { }
    if ($redirected -and -not $ReadKey) { Write-Host "$q n (no console: no)"; return $false }
    if (-not $ReadKey) { try { while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true) } } catch { } }
    $end = (Get-Date).AddSeconds($Seconds)
    $left = -1
    while ((Get-Date) -lt $end) {
        $now = [int][Math]::Ceiling(($end - (Get-Date)).TotalSeconds)
        if ($now -ne $left) { $left = $now; Write-Host -NoNewline "`r$q (no in $left s)  " }
        $key = if ($ReadKey) { & $ReadKey } elseif ([Console]::KeyAvailable) { [Console]::ReadKey($true) } else { $null }
        if ($key) {
            if ("$($key.Key)" -eq 'Y') { Write-Host 'y'; return $true }
            if ("$($key.Key)" -eq 'N' -or "$($key.Key)" -eq 'Enter') { Write-Host 'n'; return $false }
        }
        Start-Sleep -Milliseconds 100
    }
    Write-Host 'n'
    $false
}

# Sets the answer in both states: sending on turns the reports on too (the report is what is sent).
function Set-ProblemSendChoice([string]$Kit, [bool]$On) {
    $sf = Get-ProblemSendStateFile $Kit
    $s = Read-ProblemSendState $sf
    $s.send = if ($On) { 'on' } else { 'off' }
    $s.asked = (Get-Date -Format 'yyyy-MM-dd')
    if ($On -and -not $s.install) { $s.install = New-ProblemSendId }
    Write-ProblemSendState $sf $s
    # (a No here also answers problem-report.ps1's older question for a player who never had it: one question a start)
    $rf = Get-ProblemStateFile $Kit
    $r = Read-ProblemState $rf
    $choice = if ($On) { 'on' } elseif (-not $r.choice) { 'off' } else { $r.choice }
    if ($r.choice -ne $choice) { $r.choice = $choice; Write-ProblemState $rf $r }
    $s
}

# Longshot's crash records (<game>\bug-reports\pending\crash-<UTC>.txt, as coop_problem_ask.h reads them; not the
# "unclean-" ones) that no report had yet, oldest first, at most 3.
function Get-ProblemCrashRecords([string[]]$GameDirs, [string[]]$Known) {
    $found = @()
    foreach ($d in $GameDirs) {
        $p = Join-Path $d 'bug-reports\pending'
        $found += @(Get-ChildItem -LiteralPath $p -Filter 'crash-*.txt' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^crash-\d{8}T\d{6}Z' -and $Known -notcontains $_.Name })
    }
    @($found | Sort-Object Name | Select-Object -Last 3)
}

# A crash record, masked as a problem line is: Protect-LogText, players' names (those in $Names, from the sessions' logs,
# and any the record itself gives where the module writes one) as <name>, then the address gate drops a line. 64 KB at
# most (the first 4000 lines).
function Get-ProblemCrashText([string]$Path, [hashtable]$Mask, [string]$UserName = $env:USERNAME,
                              [string]$PcName = $env:COMPUTERNAME, [System.Collections.Generic.HashSet[string]]$Names = $null) {
    if ($null -eq $Names) { $Names = New-Object 'System.Collections.Generic.HashSet[string]' }
    $raw = New-Object System.Collections.Generic.List[string]
    $stream = Open-ProblemLog $Path
    try {
        $reader = New-Object System.IO.StreamReader($stream, (New-Object System.Text.UTF8Encoding $false), $true)
        try {
            while ($raw.Count -lt 4000 -and $null -ne ($line = $reader.ReadLine())) { Add-ProblemNames $line $Names; $raw.Add($line) }
        } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
    $out = New-Object System.Text.StringBuilder
    foreach ($line in $raw) {
        $m = Protect-ProblemNames (Protect-LogText $line $Mask $UserName $PcName) $Names
        if (Test-ProblemHasAddress $m) { $m = '(a line with an address was left out)' }
        if ($out.Length + $m.Length + 2 -gt $script:SendMaxCrashBytes) { [void]$out.Append("(cut)`r`n"); break }
        [void]$out.Append($m).Append("`r`n")
    }
    $out.ToString()
}

# The zip to send for one report: its problem-report.txt (from the report zip problem-report.ps1 just made, or a short
# one when only a crash is new) and the new crash records as crash-record-<n>.txt. $null when there is nothing new.
function New-ProblemSendZip([string]$ReportZip, [System.IO.FileInfo[]]$Crashes, [string]$OutZip, [string]$Version = '',
                            [string]$UserName = $env:USERNAME, [string]$PcName = $env:COMPUTERNAME,
                            [System.Collections.Generic.HashSet[string]]$Names = $null) {
    if (-not $ReportZip -and @($Crashes).Count -eq 0) { return $null }
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ("coop-send-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    try {
        $mask = New-MaskState
        $report = Join-Path $stage 'problem-report.txt'
        if ($ReportZip) {
            Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
            $z = [System.IO.Compression.ZipFile]::OpenRead($ReportZip)
            try { [System.IO.Compression.ZipFileExtensions]::ExtractToFile($z.GetEntry('problem-report.txt'), $report) }
            finally { $z.Dispose() }
        } else {
            $lines = @("Squadala! problem report: 0 new problem(s) from 0 session(s), $(@($Crashes).Count) crash record(s)",
                "Kit: $(Protect-LogText $Version $mask $UserName $PcName)",
                'Addresses, names, codes and passwords are masked (as collect-logs masks them). Sent by the kit.', '')
            [System.IO.File]::WriteAllText($report, ($lines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding $false))
        }
        $n = 0
        foreach ($c in @($Crashes)) {
            $n++
            $text = "$($c.Name -replace '-\d+-\d+(?=\.txt$)', '')`r`n" + (Get-ProblemCrashText $c.FullName $mask $UserName $PcName $Names)
            [System.IO.File]::WriteAllText((Join-Path $stage "crash-record-$n.txt"), $text, (New-Object System.Text.UTF8Encoding $false))
        }
        $dir = Split-Path -Parent $OutZip
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Write-LogZip $stage $OutZip
        $OutZip
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# The gate before an upload: '' when the zip may go, else why not. Only the kit's own names, small, UTF-8 text, and every
# line as the masking leaves it: masking it again changes nothing, no address or host is left (Test-ProblemHasAddress),
# and neither this PC's user name nor its PC name is in it (Protect-LogText's own rules for them).
function Test-ProblemSendZip([string]$Zip, [string]$UserName = $env:USERNAME, [string]$PcName = $env:COMPUTERNAME) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $info = Get-Item -LiteralPath $Zip -ErrorAction SilentlyContinue
    if (-not $info -or $info.Length -le 0 -or $info.Length -gt 200KB) { return 'size' }
    try { $z = [System.IO.Compression.ZipFile]::OpenRead($Zip) } catch { return 'not a zip' }
    try {
        if ($z.Entries.Count -lt 1 -or $z.Entries.Count -gt 4) { return 'files' }
        if (-not $z.GetEntry('problem-report.txt')) { return 'no problem-report.txt' }
        foreach ($e in $z.Entries) {
            if ($e.FullName -notmatch $script:SendNames) { return "file name $($e.FullName)" }
            if ($e.Length -gt 120KB) { return "size of $($e.FullName)" }
            $r = New-Object System.IO.StreamReader($e.Open(), (New-Object System.Text.UTF8Encoding($false, $true)))
            try { $text = $r.ReadToEnd() } catch { return "not UTF-8: $($e.FullName)" } finally { $r.Dispose() }
            foreach ($line in ($text -split "`r?`n")) {
                if (-not $line) { continue }
                if ((Protect-LogText $line (New-MaskState) $UserName $PcName) -cne $line) { return "unmasked text in $($e.FullName)" }
                if (Test-ProblemHasAddress $line) { return "an address in $($e.FullName)" }
            }
        }
    } finally { $z.Dispose() }
    ''
}

# The crash records a sent zip held ($Zip.crashes), added to their game's bug-reports\coop-sent.txt: the game's title then
# does not ask the player to send that crash with /bugreport (hookshot/src/coop_problem_ask.h: we have it already).
function Add-ProblemSentCrashes([string]$Zip) {
    $list = "$Zip.crashes"
    if (-not (Test-Path -LiteralPath $list)) { return }
    foreach ($p in @(Get-Content -LiteralPath $list -ErrorAction SilentlyContinue | Where-Object { $_ })) {
        try {
            $file = Join-Path (Split-Path -Parent (Split-Path -Parent $p)) 'coop-sent.txt'
            $have = @(Get-Content -LiteralPath $file -ErrorAction SilentlyContinue)
            $name = Split-Path -Leaf $p
            if ($have -notcontains $name) {
                $keep = @($have | Where-Object { $_ } | Select-Object -Last 199) + $name
                [System.IO.File]::WriteAllText($file, (($keep -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding $false))
            }
        } catch { }
    }
}

# One upload over HTTPS: 'sent', 'duplicate' (the bot had it), 'retry' (no network, the bot busy or away: tried again
# later) or 'refused' (the bot will never take it). TLS 1.2 or newer; $TimeoutMs for all of it.
function Send-ProblemZip([string]$Zip, [string]$Url, [string]$Install, [string]$Id, [int]$TimeoutMs = $script:SendTimeoutMs) {
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]'Tls13' } catch { }
    $bytes = [System.IO.File]::ReadAllBytes($Zip)
    $req = [System.Net.HttpWebRequest]::Create($Url)
    $req.Method = 'POST'
    $req.ContentType = 'application/zip'
    $req.ContentLength = $bytes.Length
    $req.Timeout = $TimeoutMs
    $req.ReadWriteTimeout = $TimeoutMs
    $req.AllowAutoRedirect = $false
    $req.ServicePoint.Expect100Continue = $false
    $req.UserAgent = 'Squadala-kit'
    $req.Headers.Add('X-Squadala-Install', $Install)
    $req.Headers.Add('X-Squadala-Report', $Id)
    $code = 0
    $body = ''
    try {
        $s = $req.GetRequestStream()
        try { $s.Write($bytes, 0, $bytes.Length) } finally { $s.Dispose() }
        $resp = $req.GetResponse()
        $code = [int]$resp.StatusCode
        $r = New-Object System.IO.StreamReader($resp.GetResponseStream())
        try { $body = $r.ReadToEnd() } finally { $r.Dispose(); $resp.Dispose() }
    } catch {
        # (PowerShell wraps the WebException of a .NET call: an HTTP error's answer is on the inner one)
        $ex = $_.Exception
        while ($ex -and -not ($ex -is [System.Net.WebException])) { $ex = $ex.InnerException }
        $resp = if ($ex) { $ex.Response } else { $null }
        if (-not $resp) { return @{ Result = 'retry'; Why = 'no answer' } }
        $code = [int]$resp.StatusCode
        $resp.Dispose()
    }
    if ($code -eq 200) {
        $n = ''
        try { $n = "$(($body | ConvertFrom-Json).report)" } catch { }
        if ($body -match '"duplicate"\s*:\s*true') { return @{ Result = 'duplicate'; Why = '' } }
        return @{ Result = 'sent'; Why = $(if ($n -match '^\d+$') { "report #$n" } else { '' }) }
    }
    if ($code -in 400, 404, 405, 411, 413, 415, 422) { return @{ Result = 'refused'; Why = "answer $code" } }
    @{ Result = 'retry'; Why = "answer $code" }
}

function Write-ProblemSendResult([string]$Kit, [string]$Text) {
    $dir = Get-ProblemSendDir $Kit
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm') $Text"
    [System.IO.File]::WriteAllText((Join-Path $dir 'result.txt'), "$line`r`n", (New-Object System.Text.UTF8Encoding $false))
}

# Sends the queue, oldest first, once each: a zip the gate stops or the bot refuses is moved to held\ (never sent); one
# that could not go stays for the next try (each start and each game's end), until $SendMaxTries or $SendMaxDays. With a
# 'retry' the rest waits too (no network now). Returns the results (tests).
function Send-ProblemQueue([string]$Kit, [string]$Url = (Get-ProblemSendUrl $Kit), [scriptblock]$Send = $null) {
    $dir = Get-ProblemSendDir $Kit
    $sf = Get-ProblemSendStateFile $Kit
    $results = @()
    $lock = Enter-ProblemSendLock 5 'queue'
    if (-not $lock) { return $results }
    try {
        $state = Read-ProblemSendState $sf
        if ($state.send -ne 'on' -or -not $state.install) { return $results }
        foreach ($zip in @(Get-ChildItem -LiteralPath $dir -Filter '*.zip' -File -ErrorAction SilentlyContinue | Sort-Object Name)) {
            $id = [System.IO.Path]::GetFileNameWithoutExtension($zip.Name) -replace '^.*-', ''
            if ($id -notmatch '^[0-9a-f]{32}$') { continue }
            $move = ''
            $why = Test-ProblemSendZip $zip.FullName
            if ($why) { $r = @{ Result = 'held'; Why = "stopped before sending ($why)" }; $move = 'held' }
            elseif (-not $Url) { $r = @{ Result = 'retry'; Why = 'no address to send to in this kit yet' } }
            else {
                $r = if ($Send) { & $Send $zip.FullName $Url $state.install $id } else { Send-ProblemZip $zip.FullName $Url $state.install $id }
                if ($r.Result -in 'sent', 'duplicate') { $move = 'sent' } elseif ($r.Result -eq 'refused') { $move = 'held' }
            }
            $triesFile = "$($zip.FullName).tries"
            if (-not $move) {
                $t = 1
                try { $t += [int]"$(Get-Content -LiteralPath $triesFile -TotalCount 1 -ErrorAction Stop)".Trim() } catch { }
                try { [System.IO.File]::WriteAllText($triesFile, "$t") } catch { }
                if ($t -ge $script:SendMaxTries -or $zip.CreationTime -lt (Get-Date).AddDays(-$script:SendMaxDays)) {
                    $r = @{ Result = 'gave up'; Why = "not sent after $t tries" }; $move = 'held'
                }
            }
            if ($move -eq 'sent') { Add-ProblemSentCrashes $zip.FullName }
            if ($move) {
                Remove-Item -LiteralPath "$($zip.FullName).crashes" -Force -ErrorAction SilentlyContinue
                $to = Join-Path $dir $move
                if (-not (Test-Path -LiteralPath $to)) { New-Item -ItemType Directory -Path $to -Force | Out-Null }
                Move-Item -LiteralPath $zip.FullName -Destination (Join-Path $to $zip.Name) -Force
                Remove-Item -LiteralPath $triesFile -Force -ErrorAction SilentlyContinue
                Get-ChildItem -LiteralPath $to -Filter '*.zip' -File -ErrorAction SilentlyContinue | Sort-Object Name -Descending |
                    Select-Object -Skip $script:SendKept | Remove-Item -Force -ErrorAction SilentlyContinue
            }
            $text = switch ($r.Result) {
                'sent' { "Problem report sent to the Squadala! team$(if ($r.Why) { " ($($r.Why))" }): $($zip.Name)" }
                'duplicate' { "Problem report sent to the Squadala! team (it had it already): $($zip.Name)" }
                'retry' { "Problem report not sent yet ($($r.Why)); tried again after your next game or start: $($zip.Name)" }
                'refused' { "Problem report not taken by the bot ($($r.Why)); kept in logs\problem-send\held: $($zip.Name)" }
                default { "Problem report not sent, $($r.Why); kept in logs\problem-send\held: $($zip.Name)" }
            }
            Write-ProblemSendResult $Kit $text
            $results += @{ Zip = $zip.Name; Result = $r.Result }
            if ($r.Result -eq 'retry') { break }
        }
    } finally { Exit-ProblemSendLock $lock }
    $results
}

# The report of the sessions since the last one (problem-report.ps1's New-ProblemReport, which also writes its
# coop-report-<date>.zip as before) and the new crash records, queued as logs\problem-send\<date>-<id>.zip. Returns the
# queued zip or $null (a quiet session: nothing). $Logs, $GameDirs: tests.
function Add-ProblemSendReport([string]$Kit, [string]$Version = '', [string[]]$Logs = $null, [string[]]$GameDirs = $null,
                               [string]$UserName = $env:USERNAME, [string]$PcName = $env:COMPUTERNAME, [int]$Wait = 60) {
    $lock = Enter-ProblemSendLock $Wait
    if (-not $lock) { return $null }
    try {
        if ($null -eq $Logs) { $Logs = @(Get-ProblemLogs $Kit) }
        if ($null -eq $GameDirs) { $GameDirs = @(Get-KitLogGameExes $Kit | ForEach-Object { Get-KitHookshotLogDirs $_ } | Select-Object -Unique) }
        $rf = Get-ProblemStateFile $Kit
        $rs = Read-ProblemState $rf
        $sf = Get-ProblemSendStateFile $Kit
        $ss = Read-ProblemSendState $sf
        $names = New-Object 'System.Collections.Generic.HashSet[string]'
        $report = New-ProblemReport $Logs $rs (Join-Path $Kit 'logs') $Version $UserName $PcName $names
        $crashes = @(Get-ProblemCrashRecords $GameDirs $ss.crashes)
        # (a crash record is masked with every player's name in the logs, also those of sessions read before)
        if ($crashes) { Get-ProblemLogNames $Logs $names }
        $out = Join-Path (Get-ProblemSendDir $Kit) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + (New-ProblemSendId) + '.zip')
        $zip = New-ProblemSendZip $(if ($report) { $report.Zip } else { '' }) $crashes $out $Version $UserName $PcName $names
        Write-ProblemState $rf $rs
        if ($crashes) {
            $ss.crashes = @($ss.crashes) + @($crashes | ForEach-Object { $_.Name }); Write-ProblemSendState $sf $ss
            # (which crash records this zip holds: once the bot took it, Add-ProblemSentCrashes tells the game)
            if ($zip) { [System.IO.File]::WriteAllLines("$zip.crashes", [string[]]@($crashes | ForEach-Object { $_.FullName })) }
        }
        $zip
    } finally { Exit-ProblemSendLock $lock }
}

# A hidden PowerShell window running this script with $Arguments (it never holds the start or the game up).
function Start-ProblemSendHidden([string[]]$Arguments) {
    $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$PSCommandPath`"") + $Arguments
    Start-Process -FilePath $ps -ArgumentList $args2 -WindowStyle Hidden | Out-Null
}

if ($MyInvocation.InvocationName -eq '.') { return }   # (dot-sourced: the functions only)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'log-mask.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
. (Join-Path $PSScriptRoot 'problem-report.ps1')
$kit = if ($env:OOT_COOP_KIT) { $env:OOT_COOP_KIT } else { Split-Path -Parent $PSScriptRoot }
$version = ''
try { $version = (Get-Content -LiteralPath (Join-Path $kit 'VERSION.txt') -TotalCount 1 -ErrorAction Stop) } catch { }
$sendState = Read-ProblemSendState (Get-ProblemSendStateFile $kit)

if ($Turn) {
    if ($Turn -in 'send', 'nosend') {
        [void](Set-ProblemSendChoice $kit ($Turn -eq 'send'))
        if ($Turn -eq 'send') { Write-Host 'Sending is on: after a game with a co-op problem or a crash, its masked report goes to the Squadala! team.' }
        else { Write-Host 'Sending is off: reports stay on this PC (scripts\problem-report.cmd still makes one when you ask).' }
    } elseif ($Turn -eq 'off' -and $sendState.send -eq 'on') {
        [void](Set-ProblemSendChoice $kit $false)
        Write-Host 'Sending problem reports is off too.'
    }
    return
}

if ($Watch) {
    if ($sendState.send -ne 'on') { return }
    $p = Get-Process -Id $Watch -ErrorAction SilentlyContinue
    if (-not $p) { return }
    $started = 0
    try { $started = $p.StartTime.ToUniversalTime().Ticks } catch { }
    Start-ProblemSendHidden @('-AfterGame', '-GamePid', "$Watch", '-GameStart', "$started")
    return
}

if ($AfterGame) {
    # Wait for this game (the same process: its start time too, as a process id is reused) to close, a crash too.
    while ($true) {
        $p = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
        if (-not $p) { break }
        try { if ($GameStart -and $p.StartTime.ToUniversalTime().Ticks -ne $GameStart) { break } } catch { }
        try { [void]$p.WaitForExit(600000) } catch { Start-Sleep -Seconds 30 }
        if ($p.HasExited) { break }
    }
    # Longshot's crash watcher writes its record just after the game ends; the log is closed by then too.
    Start-Sleep -Seconds 20
    try { [void](Add-ProblemSendReport $kit $version) } catch { }
    try { [void](Send-ProblemQueue $kit) } catch { }
    return
}

if ($SendPending) {
    try { [void](Send-ProblemQueue $kit) } catch { }
    return
}

if ($AtStart) {
    $url = Get-ProblemSendUrl $kit
    if (-not $sendState.send -and $url) {
        $yes = Read-ProblemSendAnswer -Url $url
        $sendState = Set-ProblemSendChoice $kit $yes
        if ($yes) { Write-Host 'Sending is on. scripts\problem-report.cmd off turns it off.' }
        else { Write-Host 'Not sent. scripts\problem-report.cmd send turns sending on later.' }
    }
    if ($sendState.send -ne 'on') {
        & (Join-Path $PSScriptRoot 'problem-report.ps1') -AtStart
        return
    }
    $last = Join-Path (Get-ProblemSendDir $kit) 'result.txt'
    if (Test-Path -LiteralPath $last) {
        $line = "$(Get-Content -LiteralPath $last -TotalCount 1)"
        if ($line) { Write-Host "Last: $line" }
    }
    # The sessions since the last report (a game whose end was not seen: the PC was shut down, the window closed).
    # (at most 5 s: a game's end making its report just now has these sessions anyway)
    try { [void](Add-ProblemSendReport $kit $version -Wait 5) } catch { }
    if (@(Get-ChildItem -LiteralPath (Get-ProblemSendDir $kit) -Filter '*.zip' -File -ErrorAction SilentlyContinue).Count -gt 0) {
        Start-ProblemSendHidden @('-SendPending')
    }
}
