# Local co-op: 2 to 4 games on this PC from one start of "Start Squadala.bat", one per player, each with its own
# controller (docs/play-lan.md, "Two to four players on one PC"). Dot-sourced by play.ps1, which asks how many players
# (Read-KitLocalPlayers) after its update and game build, then calls Start-KitLocalCoop instead of starting one game.
#
# What it does:
#  - Game 1 is the kit's own game (its own saves, coop.ini, logs). It HOSTS the session itself at its start
#    (OOT_COOP_LOCAL_HOST=port:password, the module's CoopPause LocalHostStep: a private game on 127.0.0.1 only).
#  - Games 2..N run from <game>\local-players\playerN: the game's big files hard-linked (no admin; the folder is on the
#    game's drive), its folders as junctions, the small files a game writes in place copied, and the player's own
#    user\ (saves), logs\, coop.ini (the session: 127.0.0.1, the port, the password, the name), coop-player.ini (the
#    controller and the look: the module reads the kit's coop-options.ini with it over it, so a setting changed in the
#    kit reaches every game) and player-key.txt. They join game 1 at their start (the game keeps trying until it is up).
#  - Controllers (patch 0015, read behind other windows): game 1 pad 1 and the keyboard/mouse, game N pad N with no
#    keyboard. A player's own choice in Multiplayer > Settings > Controller stays (coop-player.ini).
#  - Every game in a window, side by side (2) or in quarters (3, 4) of the primary monitor; games 2..N open without
#    taking the focus. Each keeps the player's own frame cap, uncapped as a single game (as up to 0.11.12);
#    coop-options.ini local_fps = 30 or 60 gives every game the same steady cap instead: two to four games can want
#    more than one PC gives, and Windows hands the window in front the bigger share, so on a weak PC the others get
#    uneven leftovers (row 83 / #245). Discord's status comes from game 1 only.
#  - Nobody's title is pressed: each player loads a save (or presses Continue) as in any join.
#  - Game 1 closing ends its server, so the others get the usual "the host left".

$script:KitLocalDirsNotLinked = @('user', 'logs', 'local-players', 'nucleus-players', 'bug-reports', 'playtest')
# The small files a game writes in place (not through a .tmp moved over them): each game its own copy, made once.
# (as tools/nucleus/OoTCoop.js Game.FileSymlinkCopyInstead)
$script:KitLocalCopied = @('longshot_options.ini', 'language.txt', 'longshot_pipeline_states.bin', 'longshot_compute_variants.bin',
    'longshot_computes.bin', 'longshot_module_ids.bin', 'mkw_vk_pipelines.stamp', 'longshot_prepared.bin', 'longshot_pipelines.bin.built')
# The game's own files that every game shares as they are (by name: the per-player text files the game writes beside
# its exe, coop-*.ini/.txt and the rest, stay each game's own).
$script:KitLocalLinkedPattern = '^(?i)(.+\.(exe|dll|bin|idx|png)|exefs_patches\.txt|longshot_title\.txt|coop-built-from\.txt)$'
# The folders a game does not start without (its files, the mods its built code needs).
$script:KitLocalDirsNeeded = @('romfs', 'mods')
$script:KitLocalKeys = @('LICHII_COOP_PAD', 'LICHII_COOP_KEYBOARD', 'LICHII_COOP_COLOR')

# How many players on this PC: 1 (just you, as before) or 2 to 4 (local co-op). C2-2 (owner, 0.12.0): a key is never
# the answer by itself. A solo start asks nothing and is 1 game, always. More games only after the question
# (Read-KitLocalPlayersAnswer) is answered with 2, 3 or 4; it is asked when a digit 2 to 4 was pressed while Squadala
# started (Show-KitLocalPlayersHint says so) or when the last answer (hookshot\local-players.txt, written only from an
# answer) was 2 to 4. No key, Enter, N or 1 = 1 game. OOT_COOP_LOCAL_PLAYERS (1-4) sets it outright. A start with no
# console (input from a file or a pipe: unattended) is always 1 player and waits for nothing.
function Get-KitLocalPlayersLast([string]$Kit) {
    $file = Join-Path $Kit 'hookshot\local-players.txt'
    try { if (Test-Path -LiteralPath $file) { $n = [int]"$(Get-Content -LiteralPath $file -TotalCount 1)".Trim(); if ($n -ge 1 -and $n -le 4) { return $n } } } catch { }
    return 1
}

function Test-KitLocalConsole {
    try { return -not [Console]::IsInputRedirected } catch { return $false }
}

# The digit pressed while Squadala starts (row 302: players pressed 2 and no question came). Between this hint and the
# question come other questions and key reads that throw typed-ahead keys away (the Character Select line at every
# start, the update and problem-report questions, the game build's questions in a process of its own). Each of them
# hands every key it reads or throws away to kit-paths.ps1 Save-KitLocalDigit, which keeps a 2, 3 or 4 in the file
# named here ($env:OOT_COOP_LOCAL_PRESSED: a file, so a child process's reads count too). It only makes the question
# come (C2-2). $env:OOT_COOP_LOCAL_HINT_AT: when the hint was said (UTC ticks), for the short window on a quick start.
$script:KitLocalPressedName = 'hookshot\local-players-pressed.txt'
$script:KitLocalWindowSeconds = 3
$script:KitLocalInGame = 'or in the game: the - menu, Multiplayer, Local co-op, Start the games'

function Show-KitLocalPlayersHint([string]$Kit) {
    $env:OOT_COOP_LOCAL_PRESSED = ''
    $env:OOT_COOP_LOCAL_HINT_AT = ''
    if ("$env:OOT_COOP_LOCAL_PLAYERS".Trim() -match '^[1-4]$' -or -not (Test-KitLocalConsole)) { return }
    $file = Join-Path $Kit $script:KitLocalPressedName
    try { if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force } } catch { }   # (a start before's)
    $env:OOT_COOP_LOCAL_PRESSED = $file
    $env:OOT_COOP_LOCAL_HINT_AT = "$([DateTime]::UtcNow.Ticks)"
    $last = Get-KitLocalPlayersLast $Kit
    if ($last -gt 1) { Write-Host "Local co-op: last time $last players on this PC. Squadala asks again before it starts more than one game." -ForegroundColor Cyan }
    else { Write-Host "Local co-op (2 to 4 players on this PC): press 2, 3 or 4 in this window while Squadala starts, then answer its question ($($script:KitLocalInGame))." -ForegroundColor Cyan }
}

# A digit kept by Save-KitLocalDigit since the hint: $true once, the file taken out and the keeping ended (the
# question's own keys are not kept for a next start).
function Receive-KitLocalDigit {
    $file = "$env:OOT_COOP_LOCAL_PRESSED"
    $env:OOT_COOP_LOCAL_PRESSED = ''
    if (-not $file) { return $false }
    $got = $false
    try {
        if (Test-Path -LiteralPath $file) {
            $got = "$(Get-Content -LiteralPath $file -TotalCount 1)".Trim() -match '^[2-4]$'
            Remove-Item -LiteralPath $file -Force
        }
    } catch { }
    return $got
}

# The seconds still to wait for a digit on a quick start: the hint stays on screen $script:KitLocalWindowSeconds from
# when it was said (a start with an update or a build is long past it: 0). No hint said: 0.
function Get-KitLocalWindowLeft {
    $at = "$env:OOT_COOP_LOCAL_HINT_AT"
    if ($at -notmatch '^\d+$') { return 0 }
    $gone = ([DateTime]::UtcNow - [DateTime]::new([long]$at, [DateTimeKind]::Utc)).TotalSeconds
    return [Math]::Max(0, $script:KitLocalWindowSeconds - $gone)
}

# The question (the kit's rule for a timed question, as Read-KitCharacterSelectAnswer: its line says what each key does
# and what no key does, with the seconds left; one line after says what happened). 2, 3 or 4 = that many games; 1, N,
# Enter = 1; no key in $Seconds = 1 and nothing written (no answer). Keys pressed before it are thrown away first, so the
# digit that made it ask never answers it. $ReadKey: tests (a scriptblock giving a key object or $null). Returns the
# count and whether it was answered: @{ Players; Answered }.
function Read-KitLocalPlayersAnswer([int]$Last = 1, [int]$Seconds = 15, [scriptblock]$ReadKey = $null) {
    $q = 'Players on this PC: press 2, 3 or 4 for local co-op (a game and a controller each), 1 or N for just you'
    Write-Host ''
    if ($Last -gt 1) { Write-Host "Last time: $Last players on this PC." }
    if (-not $ReadKey) { try { while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true) } } catch { } }
    $end = (Get-Date).AddSeconds($Seconds)
    $left = -1
    while ((Get-Date) -lt $end) {
        $now = [int][Math]::Ceiling(($end - (Get-Date)).TotalSeconds)
        if ($now -ne $left) { $left = $now; Write-Host -NoNewline "`r$q (no key in $left s: just you)  " }
        $key = if ($ReadKey) { & $ReadKey } elseif ([Console]::KeyAvailable) { [Console]::ReadKey($true) } else { $null }
        if ($key) {
            $c = "$($key.KeyChar)"
            if ($c -match '^[2-4]$') { Write-Host $c; return @{ Players = [int]$c; Answered = $true } }
            if ($c -eq '1' -or "$($key.Key)" -eq 'N' -or "$($key.Key)" -eq 'Enter') { Write-Host '1'; return @{ Players = 1; Answered = $true } }
        }
        Start-Sleep -Milliseconds 100
    }
    Write-Host 'no key'
    return @{ Players = 1; Answered = $false }
}

# The digit counts from wherever it was pressed since the hint: a key still waiting here, or one an earlier key read
# kept (Receive-KitLocalDigit). A quick start (no update, no build: a second or two) waits out the hint's short window
# with one line, so there is time to press it; a digit there makes it ask too. $ReadKey and $Pressed (the characters
# pressed while the start ran): tests only. $Window: tests (seconds; -1 = what is left of the hint's window).
function Read-KitLocalPlayers([string]$Kit, [int]$Seconds = 15, [scriptblock]$ReadKey = $null, [string[]]$Pressed = @(), [double]$Window = -1) {
    $given = "$env:OOT_COOP_LOCAL_PLAYERS".Trim()
    if ($given -match '^[1-4]$') { return [int]$given }
    if (-not $ReadKey -and -not (Test-KitLocalConsole)) { return 1 }
    $last = Get-KitLocalPlayersLast $Kit
    $kept = Receive-KitLocalDigit
    $asked = $last -gt 1 -or $kept
    if (-not $ReadKey) {
        try {
            while ([Console]::KeyAvailable) {   # (only what was pressed already: no wait; a digit only makes it ask)
                $k = [Console]::ReadKey($true)
                if ("$($k.KeyChar)" -match '^[2-4]$') { $asked = $true }
            }
        } catch { }
    } elseif (@($Pressed | Where-Object { "$_" -match '^[2-4]$' }).Count) { $asked = $true }   # (tests: keys pressed during the start)
    $left = if ($Window -ge 0) { $Window } else { Get-KitLocalWindowLeft }
    if (-not $asked -and $left -gt 0) {
        $end = (Get-Date).AddSeconds($left)
        $shown = -1
        while (-not $asked -and (Get-Date) -lt $end) {
            $now = [int][Math]::Ceiling(($end - (Get-Date)).TotalSeconds)
            if ($now -ne $shown) { $shown = $now; Write-Host -NoNewline "`rLocal co-op: press 2, 3 or 4 now (the game starts in $now s; $($script:KitLocalInGame))  " }
            $key = if ($ReadKey) { & $ReadKey } else { try { if ([Console]::KeyAvailable) { [Console]::ReadKey($true) } } catch { $null } }
            if ($key -and "$($key.KeyChar)" -match '^[2-4]$') { $asked = $true }
            elseif (-not $asked) { Start-Sleep -Milliseconds 100 }
        }
        Write-Host ''
    }
    if (-not $asked) { return 1 }
    $a = Read-KitLocalPlayersAnswer $last $Seconds $ReadKey
    if ($a.Answered) {
        $file = Join-Path $Kit 'hookshot\local-players.txt'
        try { New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null; [System.IO.File]::WriteAllText($file, "$($a.Players)`r`n") } catch { }
    }
    if ($a.Players -gt 1) { Write-Host "Starting $($a.Players) games (local co-op). Next start: press 1 or N at the question for just you." }
    else { Write-Host 'Starting 1 game (just you).' }
    return $a.Players
}

# A UDP port on 127.0.0.1 nobody holds, from $From up (the session's; a Host.bat server may hold the kit's 27510).
function Get-KitLocalPort([int]$From = 27560) {
    for ($p = $From; $p -lt $From + 40; $p++) {
        $u = $null
        try {
            $u = New-Object System.Net.Sockets.UdpClient (New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Loopback, $p))
            return $p
        } catch { } finally { if ($u) { $u.Close() } }
    }
    return $From
}

function New-KitLocalPassword {
    return [guid]::NewGuid().ToString('N').Substring(0, 24)
}

# The value of $Key in an ini text (the last active line), '' when none.
function Get-KitLocalIniValue([string]$Text, [string]$Key) {
    $v = ''
    foreach ($l in ($Text -split "`r?`n")) {
        $t = $l.Trim()
        if ($t -match '^[#;]') { continue }
        $eq = $t.IndexOf('=')
        if ($eq -gt 0 -and $t.Substring(0, $eq).Trim() -ieq $Key) { $v = $t.Substring($eq + 1).Trim() -replace '\s+[#;].*$', '' }
    }
    return $v
}

# $Text with $Key = $Value (the active line replaced, else added); $Spaced: "key = value", else "KEY=value".
function Set-KitLocalIniValue([string]$Text, [string]$Key, [string]$Value, [switch]$Spaced) {
    $line = if ($Spaced) { "$Key = $Value" } else { "$Key=$Value" }
    $lines = @($Text -split "`r?`n")
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^[#;]') { continue }
        $eq = $t.IndexOf('=')
        if ($eq -gt 0 -and $t.Substring(0, $eq).Trim() -ieq $Key) { $lines[$i] = if ($done) { "# $($lines[$i])" } else { $line }; $done = $true }
    }
    if (-not $done) {
        $lines = @($lines | Where-Object { $_ -ne '' }) + $line
        return ($lines -join "`r`n") + "`r`n"
    }
    return ($lines -join "`r`n")
}

# The frame cap every local game runs at: coop-options.ini local_fps = 30 or 60 (Longshot's 30 FPS cap or the
# console's 60), else 0: no cap of ours, each game keeps the player's own frame_rate_cap (uncapped, as up to 0.11.12).
function Get-KitLocalFps([string]$Kit) {
    $text = ''
    try { $text = [System.IO.File]::ReadAllText((Join-Path $Kit 'coop-options.ini')) } catch { }
    $v = Get-KitLocalIniValue $text 'local_fps'
    if ($v -eq '30') { return 30 }
    if ($v -eq '60') { return 60 }
    return 0
}

# Same file (a hard link to it, or a copy that is still the same): length and time agree.
function Test-KitLocalSame([System.IO.FileInfo]$A, [string]$B) {
    if (-not (Test-Path -LiteralPath $B -PathType Leaf)) { return $false }
    $b = Get-Item -LiteralPath $B -Force
    return $A.Length -eq $b.Length -and $A.LastWriteTimeUtc -eq $b.LastWriteTimeUtc
}

# Brings player N's folder ($Inst) up to the game's ($Game): links what is shared, copies once what a game writes in
# place, leaves the player's own files. A file the game changed since (a rebuild, an update) is linked again.
# Returns the lines to say (problems only).
function Sync-KitLocalInstance([string]$Game, [string]$Inst) {
    $said = @()
    New-Item -ItemType Directory -Force -Path $Inst | Out-Null
    foreach ($d in @(Get-ChildItem -LiteralPath $Game -Directory -Force -ErrorAction SilentlyContinue)) {
        if ($script:KitLocalDirsNotLinked -contains $d.Name -or $d.Name -like 'autotest*' -or $d.Name -like '*.old*' -or $d.Name -like '*.bak*') { continue }
        $to = Join-Path $Inst $d.Name
        $item = Get-Item -LiteralPath $to -Force -ErrorAction SilentlyContinue   # (also a link whose folder is gone)
        if ($item) {
            if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
                # (a junction made before: kept while it names this game's folder; a game folder moved since, its kit
                # copied or the game built somewhere else, left it naming the old place: made again)
                $target = "$(@($item.Target)[0])" -replace '^\\\\[?]\\|^\\[?][?]\\', ''
                $target = $target.TrimEnd('\')
                if ($target -ieq $d.FullName.TrimEnd('\')) { continue }
                try { [System.IO.Directory]::Delete($to, $false) } catch { $said += "Local co-op: could not renew the link $to ($($_.Exception.Message))."; continue }
            } else {
                # (a folder of its own, made by that game before the game's folder had one of that name: an empty one holds
                # nothing to keep and is linked; one with things in it is the player's. Never removed with its content.)
                $empty = $false
                try {
                    if (-not @(Get-ChildItem -LiteralPath $to -Force -ErrorAction Stop)) { [System.IO.Directory]::Delete($to, $false); $empty = $true }
                } catch { }
                if (-not $empty) { $said += "Local co-op: $to is a folder of its own, not the game's: left as it is."; continue }
            }
        }
        try { New-Item -ItemType Junction -Path $to -Target $d.FullName -ErrorAction Stop | Out-Null }
        catch { $said += "Local co-op: could not link $($d.Name) into $Inst ($($_.Exception.Message))." }
    }
    foreach ($f in @(Get-ChildItem -LiteralPath $Game -File -Force -ErrorAction SilentlyContinue)) {
        $to = Join-Path $Inst $f.Name
        if ($script:KitLocalCopied -contains $f.Name) {
            if (-not (Test-Path -LiteralPath $to)) { try { Copy-Item -LiteralPath $f.FullName -Destination $to -ErrorAction Stop } catch { } }
            continue
        }
        if ($f.Name -notmatch $script:KitLocalLinkedPattern) { continue }
        if (Test-KitLocalSame $f $to) { continue }
        try {
            if (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $to -Force -ErrorAction Stop }
            try { New-Item -ItemType HardLink -Path $to -Target $f.FullName -ErrorAction Stop | Out-Null }
            catch { Copy-Item -LiteralPath $f.FullName -Destination $to -ErrorAction Stop }   # (a drive without hard links)
        } catch { $said += "Local co-op: could not put $($f.Name) into $Inst ($($_.Exception.Message))." }
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $Inst 'user') | Out-Null
    return $said
}

# The game's folders as player N's game will see them (romfs, mods, ...): one line for each link that is missing or does
# not show what the game's own folder holds. Such a game starts without the game's files or mods and stops at its start:
# it is not started, and the line says why. A folder of the instance's own (Sync-KitLocalInstance leaves it) counts only
# when a game does not start without it (KitLocalDirsNeeded) and it lacks something of the game's; what it has more than
# the game's is the player's. Nothing when all is well.
function Test-KitLocalLinks([string]$Game, [string]$Inst) {
    $bad = @()
    foreach ($d in @(Get-ChildItem -LiteralPath $Game -Directory -Force -ErrorAction SilentlyContinue)) {
        if ($script:KitLocalDirsNotLinked -contains $d.Name -or $d.Name -like 'autotest*' -or $d.Name -like '*.old*' -or $d.Name -like '*.bak*') { continue }
        $to = Join-Path $Inst $d.Name
        $item = Get-Item -LiteralPath $to -Force -ErrorAction SilentlyContinue
        $own = $item -and -not ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
        if ($own -and $script:KitLocalDirsNeeded -notcontains $d.Name) { continue }
        # (read twice when something lacks: game 1 runs and may be writing in a shared folder such as cache right then)
        for ($try = 0; $try -lt 2; $try++) {
            if ($try) { Start-Sleep -Milliseconds 300 }
            $want = @(Get-ChildItem -LiteralPath $d.FullName -Force -Name -ErrorAction SilentlyContinue)
            $seen = @(Get-ChildItem -LiteralPath $to -Force -Name -ErrorAction SilentlyContinue)
            $lacks = @($want | Where-Object { $seen -notcontains $_ })
            if (-not $lacks) { break }
        }
        if (-not $lacks) { continue }
        $line = "Local co-op: $to shows $($want.Count - $lacks.Count) of the $($want.Count) things in the game's $($d.Name) folder"
        if ($own) { $bad += "$line. It is a folder of its own: move it somewhere else, then start again."; continue }
        $names = if ($item) { "$(@($item.Target)[0])" } else { 'nothing' }
        $bad += "$line (it names $names). Close every game and start again; if it says this again, run collect-logs."
    }
    return $bad
}

# Player N's own files for this session (N 2 to 4): coop.ini (the session; the name kept, else "Player N"),
# coop-player.ini (the controller: pad N and no keyboard unless the player chose another there), coop-kit.txt,
# player-key.txt (made once), longshot_options.ini (a window, no Discord status, the pad's camera; the frame cap only
# with local_fps, else the player's own stays).
function Write-KitLocalPlayer([string]$Inst, [string]$Kit, [int]$Player, [int]$Port, [string]$Password, [int]$Fps) {
    $ini = Join-Path $Inst 'coop.ini'
    $before = ''
    try { $before = [System.IO.File]::ReadAllText($ini) } catch { }
    $name = Get-KitLocalIniValue $before 'name'
    if (-not $name) { $name = "Player $Player" }
    $look = @(($before -split "`r?`n") | Where-Object { $_ -match '^\s*(color|look_[a-z_]+)\s*=' })
    $text = "# Local co-op (Start Squadala, scripts\local-coop.ps1): player $Player of this PC, written at each start.`r`n" +
        "server = 127.0.0.1`r`nport = $Port`r`nname = $name`r`npassword = $Password`r`n" + $(if ($look) { ($look -join "`r`n") + "`r`n" } else { '' })
    [System.IO.File]::WriteAllText($ini, $text)

    $own = Join-Path $Inst 'coop-player.ini'
    $mine = ''
    try { $mine = [System.IO.File]::ReadAllText($own) } catch { }
    if (-not $mine) {
        $mine = "# This player's own co-op settings (controller, keyboard, look) on a PC where several games share one kit.`r`n" +
            "# Everything else is in the kit's coop-options.ini. Written by the game.`r`n"
    }
    if (-not (Get-KitLocalIniValue $mine 'LICHII_COOP_PAD')) { $mine = Set-KitLocalIniValue $mine 'LICHII_COOP_PAD' "$Player" }
    if (-not (Get-KitLocalIniValue $mine 'LICHII_COOP_KEYBOARD')) { $mine = Set-KitLocalIniValue $mine 'LICHII_COOP_KEYBOARD' 'off' }
    [System.IO.File]::WriteAllText($own, $mine)
    [System.IO.File]::WriteAllText((Join-Path $Inst 'coop-kit.txt'), "$Kit`r`n")
    $key = Join-Path $Inst 'player-key.txt'
    if (-not (Test-Path -LiteralPath $key) -or -not "$(Get-Content -LiteralPath $key -TotalCount 1)".Trim()) {
        [System.IO.File]::WriteAllText($key, [guid]::NewGuid().ToString('N') + "`r`n")
    }
    $ls = Join-Path $Inst 'longshot_options.ini'
    $o = ''
    try { $o = [System.IO.File]::ReadAllText($ls) } catch { }
    $set = @(@('fullscreen', '0'), @('discord_status', '0'), @('mouse_camera', '0'))
    if ($Fps -gt 0) { $set += , @('frame_rate_cap', $(if ($Fps -eq 30) { '-1' } else { '0' })) }
    foreach ($kv in $set) {
        $o = Set-KitLocalIniValue $o $kv[0] $kv[1]
    }
    [System.IO.File]::WriteAllText($ls, $o)
}

# The windows' places: 2 side by side, 3 and 4 in quarters of $Work (left, top, right, bottom). [x, y, w, h] each.
function Get-KitLocalRects([int[]]$Work, [int]$Count) {
    $w = $Work[2] - $Work[0]; $h = $Work[3] - $Work[1]
    $cols = 2; $rows = if ($Count -le 2) { 1 } else { 2 }
    $cw = [int][math]::Floor($w / $cols); $ch = [int][math]::Floor($h / $rows)
    if ($rows -eq 1) {   # (side by side: 16:9 tall at most, in the middle of the height)
        $ch = [math]::Min($h, [int][math]::Floor($cw * 9 / 16) + 40)
    }
    $top = $Work[1] + [int][math]::Floor(($h - $ch * $rows) / 2)
    $out = @()
    for ($i = 0; $i -lt $Count; $i++) {
        $out += , @(($Work[0] + ($i % $cols) * $cw), ($top + [math]::Floor($i / $cols) * $ch), $cw, $ch)
    }
    return , $out
}

function Initialize-KitLocalWin32 {
    if ('KitLocalWin32' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class KitLocalWin32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, int[] r, uint c);
    public static IntPtr MainWindow(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h) && GetWindow(h, 4) == IntPtr.Zero) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static int[] WorkArea() { int[] r = new int[4]; SystemParametersInfo(0x30, 0, r, 0); return r; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, int[] r);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr m, int[] info);
    // (a window as big as its whole monitor: full screen, left as it is)
    public static bool FillsScreen(IntPtr h) {
        int[] w = new int[4]; if (!GetWindowRect(h, w)) return false;
        int[] mi = new int[10]; mi[0] = 40; if (!GetMonitorInfo(MonitorFromWindow(h, 2), mi)) return false;
        return w[0] <= mi[1] && w[1] <= mi[2] && w[2] >= mi[3] && w[3] >= mi[4];
    }
}
'@
}

# The primary monitor's work area (the screen without the taskbar): [left, top, right, bottom].
function Get-KitLocalWorkArea {
    try { Initialize-KitLocalWin32; $r = [KitLocalWin32]::WorkArea(); if ($r[2] -gt $r[0]) { return $r } } catch { }
    return @(0, 0, 1920, 1040)
}

# Puts each game's window in its place once it shows ($Seconds at most for all), without activating any.
function Set-KitLocalWindows([System.Diagnostics.Process[]]$Games, [int]$Seconds = 120) {
    try { Initialize-KitLocalWin32 } catch { return 0 }
    $rects = Get-KitLocalRects (Get-KitLocalWorkArea) $Games.Count
    $placed = @{}
    $until = (Get-Date).AddSeconds($Seconds)
    while ($placed.Count -lt $Games.Count -and (Get-Date) -lt $until) {
        for ($i = 0; $i -lt $Games.Count; $i++) {
            if ($placed[$i] -or $Games[$i].HasExited) { if ($Games[$i].HasExited) { $placed[$i] = $true }; continue }
            $h = [KitLocalWin32]::MainWindow([uint32]$Games[$i].Id)
            if ($h -eq [IntPtr]::Zero) { continue }
            $r = $rects[$i]
            [void][KitLocalWin32]::SetWindowPos($h, [IntPtr]::Zero, $r[0], $r[1], $r[2], $r[3], 0x0010 -bor 0x0004 -bor 0x0200)   # NOACTIVATE|NOZORDER|NOOWNERZORDER
            $placed[$i] = $true
        }
        Start-Sleep -Milliseconds 500
    }
    return $placed.Count
}

# The variables a game of this session starts with, set in this process for the next Start-Process ($Player 1: the
# host's; N: a joiner's, whose controller and colour come from its coop-player.ini, not the kit's for player 1).
function Set-KitLocalEnvironment([int]$Player, [int]$Port, [string]$Password, [int]$Fps) {
    foreach ($k in $script:KitLocalKeys + @('OOT_COOP_LOCAL_HOST', 'HOS_WINDOW_NOACTIVATE', 'HOS_FULLSCREEN', 'HOS_FPS_MAX')) {
        Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue
    }
    if ($Player -eq 1) {
        $env:OOT_COOP_LOCAL_HOST = "${Port}:$Password"
        $env:LICHII_COOP_PAD = '1'
        $env:LICHII_COOP_KEYBOARD = 'focus'
        $env:HOS_FULLSCREEN = '0'                 # (this run only: the player's own options file stays as it is)
        if ($Fps -gt 0) { $env:HOS_FPS_MAX = "$Fps" }   # (local_fps only: else the player's own cap)
        if ($script:KitLocalPlayer1Color) { $env:LICHII_COOP_COLOR = $script:KitLocalPlayer1Color }
    } else {
        $env:HOS_WINDOW_NOACTIVATE = '1'          # (opens behind: the focus stays where it is)
    }
}

# Notes the games of this session for collect-logs.cmd: <game>\local-players\instances.txt ("<player>=<folder>").
function Write-KitLocalInstances([string]$Game, [string[]]$Folders) {
    $lines = @()
    for ($i = 0; $i -lt $Folders.Count; $i++) { $lines += "$($i + 2)=$($Folders[$i])" }
    [System.IO.File]::WriteAllText((Join-Path $Game 'local-players\instances.txt'), (($lines -join "`r`n") + "`r`n"))
}

# The whole start: $Players games of $Exe (the kit's built game). Returns the game processes (game 1 first).
function Start-KitLocalCoop([string]$Kit, [string]$Exe, [int]$Players, [int]$Pause = 12) {
    $game = Split-Path -Parent $Exe
    $root = Join-Path $game 'local-players'
    $running = @(Get-Process longshot -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith(($root.TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase) })
    if ($running) {
        Write-Host 'Local co-op: games of an earlier local session still run. Close them first, then start again.' -ForegroundColor Yellow
        return @()
    }
    $port = Get-KitLocalPort
    $password = New-KitLocalPassword
    $fps = Get-KitLocalFps $Kit
    $script:KitLocalPlayer1Color = "$env:LICHII_COOP_COLOR"
    $folders = @()
    for ($p = 2; $p -le $Players; $p++) {
        $inst = Join-Path $root "player$p"
        foreach ($l in @(Sync-KitLocalInstance $game $inst)) { Write-Host $l -ForegroundColor Yellow }
        Write-KitLocalPlayer $inst $Kit $p $port $password $fps
        $blind = @(Test-KitLocalLinks $game $inst)
        if ($blind) {
            foreach ($l in $blind) { Write-Host $l -ForegroundColor Yellow }
            Write-Host "Local co-op: player $p's game would start without the game's files, so local co-op is not started." -ForegroundColor Yellow
            return @()
        }
        $folders += $inst
    }
    Write-KitLocalInstances $game $folders
    $ram = [math]::Round(((Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).TotalPhysicalMemory) / 1GB)
    if ($ram -and $ram -lt 10 * $Players) {
        Write-Host "Local co-op: each game needs about 10 GB of RAM; this PC has $ram GB for $Players games. It may run slowly." -ForegroundColor Yellow
    }
    Write-Host "Local co-op: $Players games on this PC. Game 1 (keyboard and controller 1) hosts; games 2-$Players (controllers 2-$Players) join it."
    if ($fps -gt 0) { Write-Host "  Every game runs at $fps FPS (coop-options.ini local_fps), so the windows not in front stay smooth." }
    Write-Host '  In each game, load your save or press Continue as usual. Closing game 1 ends the session.'
    $games = @()
    $saved = @{}
    foreach ($k in $script:KitLocalKeys + @('OOT_COOP_LOCAL_HOST', 'HOS_WINDOW_NOACTIVATE', 'HOS_FULLSCREEN', 'HOS_FPS_MAX')) { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
    try {
        for ($p = 1; $p -le $Players; $p++) {
            Set-KitLocalEnvironment $p $port $password $fps
            $exeN = if ($p -eq 1) { $Exe } else { Join-Path $folders[$p - 2] (Split-Path -Leaf $Exe) }
            $g = Start-Process -FilePath $exeN -WorkingDirectory (Split-Path -Parent $exeN) -PassThru
            $games += $g
            if ($p -lt $Players) { Start-Sleep -Seconds $Pause }   # (game 1 first: it hosts; the first player in hosts)
        }
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    $n = Set-KitLocalWindows $games
    Write-Host "Local co-op: $n of $Players windows placed."
    return $games
}

# The player (2 to 9) of a game whose exe is $Path when it is <game>\local-players\playerN\...; 0 for any other path.
# ($Root is <game>\local-players\ with its last \. In a .NET pattern the folder's \ is \\: one \ at its end does not parse.)
function Get-KitLocalPlayerOfPath([string]$Root, [string]$Path) {
    if (-not $Root -or -not $Path -or -not $Path.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) { return 0 }
    if ($Path.Substring($Root.Length) -match '^(?i)player([2-9])\\') { return [int]$Matches[1] }
    return 0
}

# The players 2..N whose game runs now (a longshot.exe started from <game>\local-players\playerN).
function Get-KitLocalRunning([string]$Game) {
    $root = (Join-Path $Game 'local-players').TrimEnd('\') + '\'
    $out = @()
    foreach ($p in @(Get-Process longshot -ErrorAction SilentlyContinue)) {
        $n = Get-KitLocalPlayerOfPath $root "$($p.Path)"
        if ($n) { $out += $n }
    }
    return @($out | Sort-Object -Unique)
}

# Started from game 1 (its Local co-op page, Start the games / Start missing games: the module runs
# local-coop-start.ps1): game 1 runs already and hosts the session on $Port with $Password; the games of players 2 to
# $Players that do not run are started into it, as Start-KitLocalCoop starts them (the same folders, files, controllers,
# frame cap), and placed in their part of the screen (game 1's own window is moved too unless it fills the screen).
# A game that runs already is left as it is (its player is in, or joins again from its own menu). $Result gets what
# happened, for the game's page: started=2,3 / running=4 / failed=... / problem=<one line>. Returns the started games.
function Add-KitLocalCoop([string]$Kit, [string]$Exe, [int]$Players, [int]$Port, [string]$Password, [int]$HostPid, [string]$Result) {
    $game = Split-Path -Parent $Exe
    $Players = [math]::Max(2, [math]::Min(4, $Players))
    $running = @(Get-KitLocalRunning $game)
    $fps = Get-KitLocalFps $Kit
    $folders = @()
    $want = @()
    $cannot = @()   # (a player whose folder does not show the game's: not started, failed)
    $problem = ''
    $note = ''   # (what Sync says and the game starts anyway: said only when nothing failed)
    for ($p = 2; $p -le $Players; $p++) {
        $inst = Join-Path (Join-Path $game 'local-players') "player$p"
        $folders += $inst
        if ($running -contains $p) { continue }
        foreach ($l in @(Sync-KitLocalInstance $game $inst)) { Write-Host $l; if (-not $note) { $note = $l } }
        Write-KitLocalPlayer $inst $Kit $p $Port $Password $fps
        $blind = @(Test-KitLocalLinks $game $inst)
        if ($blind) { $blind | ForEach-Object { Write-Host $_ }; $problem = $blind[0]; $cannot += $p; continue }
        $want += $p
    }
    # (players past $Players that run stay listed, so collect-logs still takes their logs)
    foreach ($r in $running) { if ($r -gt $Players) { $folders += (Join-Path (Join-Path $game 'local-players') "player$r") } }
    Write-KitLocalInstances $game $folders
    $ram = 0
    try { $ram = [math]::Round(((Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory) / 1GB) } catch { }
    $slow = if ($ram -and $ram -lt 10 * $Players) { "This PC has $ram GB of RAM for $Players games: it may run slowly." } else { '' }
    $started = @(); $failed = @($cannot); $games = @()
    $saved = @{}
    foreach ($k in $script:KitLocalKeys + @('OOT_COOP_LOCAL_HOST', 'HOS_WINDOW_NOACTIVATE', 'HOS_FULLSCREEN', 'HOS_FPS_MAX')) { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
    try {
        foreach ($p in $want) {
            Set-KitLocalEnvironment $p $Port $Password $fps
            $exeN = Join-Path $folders[$p - 2] (Split-Path -Leaf $Exe)
            $g = $null
            try { $g = Start-Process -FilePath $exeN -WorkingDirectory (Split-Path -Parent $exeN) -PassThru -ErrorAction Stop } catch { if (-not $problem) { $problem = "Player $p's game did not start: $($_.Exception.Message)" } }
            if ($g) { $started += $p; $games += $g } else { $failed += $p }
        }
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    if (-not $problem) { $problem = if ($note) { $note } else { $slow } }   # (a game that did not start says more than the RAM)
    $problem = ($problem -replace '[\r\n]+', ' ').Trim()   # (one line of the result file)
    $line = "started=$($started -join ',')`r`nrunning=$(@($running | Where-Object { $_ -le $Players }) -join ',')`r`nfailed=$($failed -join ',')`r`nproblem=$problem`r`n"
    if ($Result) { try { [System.IO.File]::WriteAllText($Result, $line) } catch { } }
    # The windows: every game of the session in its part of the screen (game 1 too when it is in a window).
    try {
        Initialize-KitLocalWin32
        $rects = Get-KitLocalRects (Get-KitLocalWorkArea) $Players
        if ($HostPid -gt 0) {
            $h = [KitLocalWin32]::MainWindow([uint32]$HostPid)
            if ($h -ne [IntPtr]::Zero -and -not [KitLocalWin32]::FillsScreen($h)) {
                $r = $rects[0]
                [void][KitLocalWin32]::SetWindowPos($h, [IntPtr]::Zero, $r[0], $r[1], $r[2], $r[3], 0x0010 -bor 0x0004 -bor 0x0200)
            }
        }
        $until = (Get-Date).AddSeconds(120)
        $placed = @{}
        while ($placed.Count -lt $games.Count -and (Get-Date) -lt $until) {
            for ($i = 0; $i -lt $games.Count; $i++) {
                if ($placed[$i]) { continue }
                if ($games[$i].HasExited) { $placed[$i] = $true; continue }
                $h = [KitLocalWin32]::MainWindow([uint32]$games[$i].Id)
                if ($h -eq [IntPtr]::Zero) { continue }
                $r = $rects[$started[$i] - 1]
                [void][KitLocalWin32]::SetWindowPos($h, [IntPtr]::Zero, $r[0], $r[1], $r[2], $r[3], 0x0010 -bor 0x0004 -bor 0x0200)
                $placed[$i] = $true
            }
            Start-Sleep -Milliseconds 500
        }
    } catch { }
    return $games
}
