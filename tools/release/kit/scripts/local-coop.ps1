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
#    taking the focus. Each runs at the same steady frame cap (coop-options.ini local_fps, 30 by default): two to four
#    games want more than one PC gives, and Windows hands the window in front the bigger share, so the others got
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
$script:KitLocalKeys = @('LICHII_COOP_PAD', 'LICHII_COOP_KEYBOARD', 'LICHII_COOP_COLOR')

# How many players on this PC: 1 (just you, as before) or 2 to 4 (local co-op). Nothing waits for an answer, so a solo
# start is not slower: Start Squadala says at its start which count it will use (Show-KitLocalPlayersHint: the last one,
# hookshot\local-players.txt, 1 the first time) and a digit 1 to 4 pressed in its window while it checks, updates or
# builds picks another (the last one pressed counts; it is kept for the next start). OOT_COOP_LOCAL_PLAYERS (1-4) sets it
# outright. A start with no console (input from a file or a pipe: unattended) is always 1 player.
function Get-KitLocalPlayersLast([string]$Kit) {
    $file = Join-Path $Kit 'hookshot\local-players.txt'
    try { if (Test-Path -LiteralPath $file) { $n = [int]"$(Get-Content -LiteralPath $file -TotalCount 1)".Trim(); if ($n -ge 1 -and $n -le 4) { return $n } } } catch { }
    return 1
}

function Test-KitLocalConsole {
    try { return -not [Console]::IsInputRedirected } catch { return $false }
}

function Show-KitLocalPlayersHint([string]$Kit) {
    if ("$env:OOT_COOP_LOCAL_PLAYERS".Trim() -match '^[1-4]$' -or -not (Test-KitLocalConsole)) { return }
    $last = Get-KitLocalPlayersLast $Kit
    $now = if ($last -eq 1) { '1 (just you)' } else { "$last (local co-op: a game and a controller each)" }
    Write-Host "Players on this PC: $now. Press 1, 2, 3 or 4 in this window while Squadala starts to change it." -ForegroundColor Cyan
}

function Read-KitLocalPlayers([string]$Kit) {
    $given = "$env:OOT_COOP_LOCAL_PLAYERS".Trim()
    if ($given -match '^[1-4]$') { return [int]$given }
    if (-not (Test-KitLocalConsole)) { return 1 }
    $last = Get-KitLocalPlayersLast $Kit
    $answer = $last
    try {
        while ([Console]::KeyAvailable) {   # (only what was pressed already: no wait)
            $k = [Console]::ReadKey($true)
            if ("$($k.KeyChar)" -match '^[1-4]$') { $answer = [int]"$($k.KeyChar)" }
        }
    } catch { }
    if ($answer -ne $last) {
        $file = Join-Path $Kit 'hookshot\local-players.txt'
        try { New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null; [System.IO.File]::WriteAllText($file, "$answer`r`n") } catch { }
    }
    if ($answer -gt 1) { Write-Host "Players on this PC: $answer. (Press 1 while Squadala starts next time for just you.)" }
    return $answer
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

# The frame cap every local game runs at: coop-options.ini local_fps = 30 or 60 (30 by default and for anything else).
function Get-KitLocalFps([string]$Kit) {
    $text = ''
    try { $text = [System.IO.File]::ReadAllText((Join-Path $Kit 'coop-options.ini')) } catch { }
    if ((Get-KitLocalIniValue $text 'local_fps') -eq '60') { return 60 }
    return 30
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
                $said += "Local co-op: $to is a folder of its own, not the game's: left as it is."
                continue
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

# Player N's own files for this session (N 2 to 4): coop.ini (the session; the name kept, else "Player N"),
# coop-player.ini (the controller: pad N and no keyboard unless the player chose another there), coop-kit.txt,
# player-key.txt (made once), longshot_options.ini (a window, no Discord status, the pad's camera, the frame cap).
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
    foreach ($kv in @(@('fullscreen', '0'), @('discord_status', '0'), @('mouse_camera', '0'), @('frame_rate_cap', $(if ($Fps -eq 30) { '-1' } else { '0' })))) {
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
        $env:HOS_FPS_MAX = "$Fps"
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
        $folders += $inst
    }
    Write-KitLocalInstances $game $folders
    $ram = [math]::Round(((Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).TotalPhysicalMemory) / 1GB)
    if ($ram -and $ram -lt 10 * $Players) {
        Write-Host "Local co-op: each game needs about 10 GB of RAM; this PC has $ram GB for $Players games. It may run slowly." -ForegroundColor Yellow
    }
    Write-Host "Local co-op: $Players games on this PC. Game 1 (keyboard and controller 1) hosts; games 2-$Players (controllers 2-$Players) join it."
    Write-Host "  Every game runs at $fps FPS (coop-options.ini local_fps), so the windows not in front stay smooth."
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
