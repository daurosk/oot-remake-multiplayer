# Problem reports (docs/problem-reports.md): the co-op module's problems from the player's finished game sessions, masked
# as collect-logs masks them (log-mask.ps1 Protect-LogText: addresses, MAC addresses, the user and PC names, invite codes,
# passwords), players' names masked too, each problem in one report only, written as logs\coop-report-<date>.zip for the
# player to send with the /report bot command. Nothing is sent from here: no webhook, no network. Sending them, when the
# player said yes, is problem-send.ps1's (it makes them with these functions).
#
# Opt-in, asked once: start.ps1 calls it with -AtStart, which asks the first time (default no) and remembers the answer in
# logs\problem-reports.json; with yes, each start makes a zip when the sessions since the last one had a new problem.
# problem-report.cmd (-Now) makes one at once, whatever the answer (the player asked for it); problem-report.cmd on (or
# off) changes the answer; off turns sending off too. problem-report.cmd send (or nosend) turns sending on or off.
# Dot-source it for its functions (tools\release\tests\problem_report_test.ps1); run as a script for the above.
param([Parameter(Position = 0)][ValidateSet('on', 'off', 'send', 'nosend')][string]$Turn, [switch]$AtStart, [switch]$Now)

# The module's problems: its own warnings and errors, lines that say something failed, was refused, gave up, timed out or
# was lost, faults the guard caught, and crashes. The words are whole words, as the module writes them (lower case), so a
# scene's name (Lost Woods) or a note that only mentions a crash is not a problem. Matched case-sensitively (-cmatch).
$script:ProblemLine = '(?i:warning|error): coop|coop: .*\b(?:fail\w*|could not|couldn''t|refus\w*|not taken|gave up|' +
    'giving up|stuck|lost|timed out|not written|not sent|damaged|mismatch\w*|wrong)\b|coop: guard: a fault|' +
    '\bCRASH\b|\bcrash record\b|\b(?i:fatal error|unhandled exception)\b'
# Routine status lines that only contain such words (counters a second or every 10 s, notes of normal states)
$script:ProblemRoutine = 'coop: pose: |coop: actors: in \d+ s|invite code is made|camera lost, markers off|' +
    'coop: flags: (by the game''s events|native)|so far; |coop: objects: list head|/s.*/s'
# Where the module writes a player's name: player 2 joined: Mido, player 2 left: Mido (...) (the server), player 2 'Mido'
# joined (a game), Mods: player 2 (Mido), name 'Mido' (our own)
$script:ProblemNameAt = 'player \d+ (?:joined|left): (?<n>[^(]+?)(?= \(|$)|player \d+ ''(?<n>[^'']+)''|' +
    'Mods: player \d+ \((?<n>[^)]+)\)|name ''(?<n>[^'']+)'''
# Dotted words that are file names, not host names (the gate)
$script:ProblemFileExt = '^(?i:exe|dll|txt|log|ini|json|zip|cpp|hpp|h|ps1|cmd|bat|pak|sav|bin|png|dds|cfg|toml|md|nup|pdb)$'
# How many report zips are kept in logs\ (the oldest go)
$script:ProblemZipsKept = 10

function Get-ProblemStateFile([string]$Kit) { Join-Path $Kit 'logs\problem-reports.json' }

# The answer (on, off, or '' when never asked), the sessions read and the problems reported (oldest first), from the state
# file.
function Read-ProblemState([string]$Path) {
    $state = @{ choice = ''; runs = @(); sent = [ordered]@{} }
    try {
        $j = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($j.choice -in @('on', 'off')) { $state.choice = $j.choice }
        if ($j.runs) { $state.runs = @($j.runs) }
        if ($j.sent) { foreach ($p in $j.sent.PSObject.Properties) { $state.sent[$p.Name] = [string]$p.Value } }
    } catch { }
    $state
}

# Keeps the newest 200 sessions and 5000 problems (sent is in the order they were reported).
function Write-ProblemState([string]$Path, [hashtable]$State) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $runs = @($State.runs | Select-Object -Last 200)
    $sent = [ordered]@{}
    foreach ($k in @($State.sent.Keys | Select-Object -Last 5000)) { $sent[$k] = $State.sent[$k] }
    $json = [ordered]@{ choice = $State.choice; runs = $runs; sent = $sent } | ConvertTo-Json -Depth 3
    $tmp = "$Path.new"
    [System.IO.File]::WriteAllText($tmp, $json, (New-Object System.Text.UTF8Encoding $false))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

# Compiled once: a log is read a line at a time, and Windows PowerShell's per-line cost is most of a start's wait.
$script:ProblemLineRx = New-Object regex $script:ProblemLine, 'Compiled'
$script:ProblemRoutineRx = New-Object regex $script:ProblemRoutine, 'Compiled, IgnoreCase'

function Test-ProblemLine([string]$Line) {
    $script:ProblemLineRx.IsMatch($Line) -and -not $script:ProblemRoutineRx.IsMatch($Line)
}

# Players' names: the ones a line names where the module writes a name are added to $Names; every name known becomes
# <name> (in this line, and in later lines that name a player bare).
function Protect-ProblemNames([string]$Text, [System.Collections.Generic.HashSet[string]]$Names) {
    foreach ($m in [regex]::Matches($Text, $script:ProblemNameAt)) {
        $n = $m.Groups['n'].Value.Trim()
        if ($n.Length -ge 2 -and $n -notmatch '^<[a-z]+(-\d+)?>$') { [void]$Names.Add($n) }
    }
    foreach ($n in @($Names | Sort-Object Length -Descending)) {
        $Text = [regex]::Replace($Text, '(?<!\w)' + [regex]::Escape($n) + '(?!\w)', '<name>')
    }
    $Text
}

# The problem a masked line is, the same for every occurrence: its thread tag, the masks' numbers (<lan-1>, <lan-2>),
# addresses and numbers blanked (names are <name> already).
function Get-ProblemFingerprint([string]$Masked) {
    $s = $Masked -replace '^\[[^\]]*\]\s*', ''
    $s = $s -replace '<([a-z]+)-\d+>', '<$1>'
    $s = $s -replace '0x[0-9a-fA-F]+|\b[0-9a-f]{8,}\b', '#'
    $s = $s -replace '\d+(\.\d+)?', '#'
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try { -join ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($s))[0..7] | ForEach-Object { $_.ToString('x2') }) }
    finally { $sha.Dispose() }
}

# A last gate after masking: anything Protect-LogText would still call an address (a mask it missed), a host name
# (myhost.duckdns.org) or a host with a port (myhost:27510) drops the line.
function Test-ProblemHasAddress([string]$Masked) {
    foreach ($m in [regex]::Matches($Masked, '(?<![\d.])\d{1,3}(?:\.\d{1,3}){3}(?![\d])')) {
        if (Get-MaskKind4 $m.Value) { return $true }
    }
    foreach ($m in [regex]::Matches($Masked, '(?<![\w:.])(?:[0-9A-Fa-f]{0,4}:){2,7}(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{0,4})(?:%[\w.-]+)?')) {
        if (Get-MaskKind6 $m.Value) { return $true }
    }
    if ($Masked -match '(?<![\w.<>-])[A-Za-z0-9][\w-]*(?:\.[\w-]+)*:\d{2,5}(?!\d)') { return $true }
    foreach ($m in [regex]::Matches($Masked, '(?<![\w.\\/-])(?:[A-Za-z0-9-]+\.)+(?<tld>[A-Za-z]{2,63})(?![\w-]|\.\w)')) {
        if ($m.Groups['tld'].Value -notmatch $script:ProblemFileExt) { return $true }
    }
    $false
}

# A log opened as collect-logs opens it: another process may still have it open for writing, or delete it.
function Open-ProblemLog([string]$Path) {
    [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
        [System.IO.FileShare]'ReadWrite, Delete')
}

# A session's key from the start of its log only: its run_id line, else its size and first 64 KB (the stream is put back
# at its start).
function Get-ProblemRunKey([System.IO.Stream]$Stream) {
    $length = $Stream.Length
    $buf = New-Object byte[] ([int][Math]::Min(65536, $length))
    $got = 0
    while ($got -lt $buf.Length) {
        $n = $Stream.Read($buf, $got, $buf.Length - $got)
        if ($n -le 0) { break }
        $got += $n
    }
    $Stream.Position = 0
    if ([System.Text.Encoding]::UTF8.GetString($buf, 0, $got) -match 'run_id: ([0-9a-f]{16,})') { return $Matches[1] }
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try { 'file-' + $length + '-' + (-join ($sha.ComputeHash($buf, 0, $got)[0..9] | ForEach-Object { $_.ToString('x2') })) }
    finally { $sha.Dispose() }
}

# The players' names a line gives where the module writes a name (Protect-ProblemNames' patterns), added to $Names.
function Add-ProblemNames([string]$Line, [System.Collections.Generic.HashSet[string]]$Names) {
    if ($Line.Contains('player ') -or $Line.Contains("name '")) { [void](Protect-ProblemNames $Line $Names) }
}

# The players' names in whole logs (the name lines only, nothing kept but the names): for text that is not a log line of
# a session read now (a crash record after a session read before). A log that cannot be read is skipped.
function Get-ProblemLogNames([string[]]$Logs, [System.Collections.Generic.HashSet[string]]$Names) {
    foreach ($log in $Logs) {
        try {
            $stream = Open-ProblemLog $log
            try {
                $reader = New-Object System.IO.StreamReader($stream, (New-Object System.Text.UTF8Encoding $false), $true)
                try { while ($null -ne ($line = $reader.ReadLine())) { Add-ProblemNames $line $Names } }
                finally { $reader.Dispose() }
            } finally { $stream.Dispose() }
        } catch { }
    }
}

# One session's log: its key and its problems, masked. A session in $Known is not read past its start (Events is $null);
# the rest is read a line at a time. Players' names are masked once the whole session is read, with every name it gave
# (a problem line before the player's join line too) and those in $Names (other sessions read now); the session's names
# are added to $Names.
function Read-ProblemRun([string]$Path, [hashtable]$Mask, [string]$UserName = $env:USERNAME,
                         [string]$PcName = $env:COMPUTERNAME, [string[]]$Known = @(),
                         [System.Collections.Generic.HashSet[string]]$Names = $null) {
    if ($null -eq $Names) { $Names = New-Object 'System.Collections.Generic.HashSet[string]' }
    $stream = Open-ProblemLog $Path
    try {
        $key = Get-ProblemRunKey $stream
        if ($Known -contains $key) { return @{ Key = $key; Events = $null } }
        $lines = [ordered]@{}   # each problem line once (address-masked), with its count
        $reader = New-Object System.IO.StreamReader($stream, (New-Object System.Text.UTF8Encoding $false), $true)
        $isProblem = $script:ProblemLineRx
        $isRoutine = $script:ProblemRoutineRx
        try {
            while ($null -ne ($line = $reader.ReadLine())) {
                # (Test-ProblemLine, inline: a call a line is most of the time on a large log)
                if ($line.Contains('player ') -or $line.Contains("name '")) { [void](Protect-ProblemNames $line $Names) }
                if (-not $isProblem.IsMatch($line) -or $isRoutine.IsMatch($line)) { continue }
                $t = $line.Trim()
                if ($lines.Contains($t)) { $lines[$t]++ } elseif ($lines.Count -lt 20000) { $lines[$t] = 1 }
            }
        } finally { $reader.Dispose() }
        $events = [ordered]@{}
        foreach ($t in $lines.Keys) {
            $masked = Protect-ProblemNames (Protect-LogText $t $Mask $UserName $PcName) $Names
            if (Test-ProblemHasAddress $masked) { continue }
            $fp = Get-ProblemFingerprint $masked
            if ($events.Contains($fp)) { $events[$fp].Count += $lines[$t] }
            else { $events[$fp] = @{ Line = $masked.Substring(0, [Math]::Min(300, $masked.Length)); Count = $lines[$t] } }
        }
        @{ Key = $key; Events = $events }
    } finally { $stream.Dispose() }
}

# The new problems of the logs not read before, as a zip in $OutDir (coop-report-<date>.zip; the newest
# $ProblemZipsKept are kept), or $null when there is none. A log that cannot be read is skipped (and tried again next
# time); the others are still read. The state's runs and sent are brought up to date (the caller writes it).
# $Names: the players' names met (filled; problem-send.ps1 masks the crash records with them).
function New-ProblemReport([string[]]$Logs, [hashtable]$State, [string]$OutDir, [string]$Version = '',
                           [string]$UserName = $env:USERNAME, [string]$PcName = $env:COMPUTERNAME,
                           [System.Collections.Generic.HashSet[string]]$Names = $null) {
    if ($null -eq $Names) { $Names = New-Object 'System.Collections.Generic.HashSet[string]' }
    $mask = New-MaskState
    $new = [ordered]@{}
    $read = @()
    foreach ($log in $Logs) {
        if (-not (Test-Path -LiteralPath $log)) { continue }
        try { $run = Read-ProblemRun $log $mask $UserName $PcName (@($State.runs) + $read) $Names }
        catch { continue }
        if ($null -eq $run.Events) { continue }
        $read += $run.Key
        foreach ($fp in $run.Events.Keys) {
            if ($State.sent.Contains($fp)) { continue }
            if ($new.Contains($fp)) { $new[$fp].Count += $run.Events[$fp].Count }
            else { $new[$fp] = $run.Events[$fp] }
        }
    }
    $State.runs = @($State.runs) + $read
    if ($new.Count -eq 0) { return $null }
    if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ("coop-report-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    try {
        $lines = @("Squadala! problem report: $($new.Count) new problem(s) from $($read.Count) session(s)",
            "Kit: $(Protect-LogText $Version $mask $UserName $PcName)",
            'Addresses, names, codes and passwords are masked (as collect-logs masks them). Send with /bugreport.', '')
        foreach ($e in @($new.Values | Sort-Object { -$_.Count })) { $lines += ('{0}x {1}' -f $e.Count, $e.Line) }
        [System.IO.File]::WriteAllText((Join-Path $stage 'problem-report.txt'), ($lines -join "`r`n") + "`r`n",
            (New-Object System.Text.UTF8Encoding $false))
        $zip = Join-Path $OutDir ('coop-report-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip')
        Write-LogZip $stage $zip
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $OutDir -Filter 'coop-report-*.zip' -File -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -Skip $script:ProblemZipsKept |
        Remove-Item -Force -ErrorAction SilentlyContinue
    foreach ($fp in $new.Keys) { $State.sent[$fp] = (Get-Date -Format 'yyyy-MM-dd') }
    @{ Zip = $zip; Count = $new.Count }
}

# The game's finished sessions' logs, where collect-logs looks (Get-KitLogGameExes, Get-KitHookshotLogDirs): the running
# session's own longshot_log.txt is left for next time (it is still being written).
function Get-ProblemLogs([string]$Kit) {
    $files = @()
    foreach ($exe in @(Get-KitLogGameExes $Kit)) {
        foreach ($dir in @(Get-KitHookshotLogDirs $exe)) {
            $files += @(Get-KitHookshotLogFiles $dir | ForEach-Object { $_.File })
        }
    }
    $files += @(Get-ChildItem (Join-Path $Kit 'logs') -Filter 'longshot_log*.txt' -File -ErrorAction SilentlyContinue)
    $running = @(Get-Process longshot, hookshot -ErrorAction SilentlyContinue).Count -gt 0
    @($files | Where-Object { -not ($running -and $_.Name -match '^(longshot|hookshot)_log\.txt$') } |
        Sort-Object LastWriteTime | ForEach-Object { $_.FullName } | Select-Object -Unique)
}

if ($MyInvocation.InvocationName -eq '.') { return }   # (dot-sourced: the functions only)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'log-mask.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$kit = if ($env:OOT_COOP_KIT) { $env:OOT_COOP_KIT } else { Split-Path -Parent $PSScriptRoot }
$stateFile = Get-ProblemStateFile $kit
if ($Turn -in 'send', 'nosend') { & (Join-Path $PSScriptRoot 'problem-send.ps1') $Turn; return }
# (the lock problem-send.ps1 makes its reports under: a game's end may be making one now)
$lock = New-Object System.Threading.Mutex($false, 'Local\SquadalaProblem-reports')
try { [void]$lock.WaitOne(30000) } catch [System.Threading.AbandonedMutexException] { }
$state = Read-ProblemState $stateFile
if ($Turn) {
    $state.choice = $Turn
    Write-ProblemState $stateFile $state
    if ($Turn -eq 'on') { Write-Host 'Problem reports are on: each start makes one when your games had a new problem.' }
    else { Write-Host 'Problem reports are off. scripts\problem-report.cmd still makes one when you ask.' }
    try { $lock.ReleaseMutex() } catch { }
    if ($Turn -eq 'off') { & (Join-Path $PSScriptRoot 'problem-send.ps1') off }
    return
}
if (-not $state.choice) {
    $interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
    if (-not $interactive) { return }
    Write-Host ''
    Write-Host 'Problem reports: after your games, the kit can put the co-op problems it finds in your logs into a small zip'
    Write-Host 'for you to send with /bugreport in the Squadala! Discord. Addresses, names and codes are masked; nothing is'
    Write-Host 'sent by itself. Change it later with scripts\problem-report.cmd on (or off).'
    $answer = Read-Host 'Make problem reports? [y/N]'
    $state.choice = if ($answer -and $answer.Trim() -match '^(?i)(y|yes)$') { 'on' } else { 'off' }
    Write-ProblemState $stateFile $state
}
if ($AtStart -and $state.choice -ne 'on') { return }
$version = ''
try { $version = (Get-Content -LiteralPath (Join-Path $kit 'VERSION.txt') -TotalCount 1 -ErrorAction Stop) } catch { }
try {
    $report = New-ProblemReport @(Get-ProblemLogs $kit) $state (Join-Path $kit 'logs') $version
    Write-ProblemState $stateFile $state
} finally { try { $lock.ReleaseMutex() } catch { } }
if ($report) {
    Write-Host "Problem report: $($report.Count) new problem(s) in $($report.Zip)"
    Write-Host 'Send it with /bugreport in the Squadala! Discord (addresses, names and codes are masked in it).'
} elseif ($Now) {
    Write-Host 'Problem report: no new problem in your game logs since the last report.'
}
