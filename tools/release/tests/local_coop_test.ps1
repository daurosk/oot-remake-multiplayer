# Tests for local co-op (tools/release/kit/scripts/local-coop.ps1: 2 to 4 games on one PC from Start Squadala).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\local_coop_test.ps1
# A made-up game folder in %TEMP% (no game, nothing started): the players question, the instance folders (links, copies,
# each player's own files, a rebuilt file linked again), each player's files for the session (coop.ini, coop-player.ini
# with the controller kept when the player chose one, longshot_options.ini), the frame cap, the variables each game
# starts with, the windows' places, and that play.ps1 and the kit use it. Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$release = Split-Path -Parent $PSScriptRoot
$scripts = Join-Path $release 'kit\scripts'
. (Join-Path $scripts 'local-coop.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Write-File([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text)
}
function Read-File([string]$Path) { if (Test-Path -LiteralPath $Path) { [System.IO.File]::ReadAllText($Path) } else { '<none>' } }
function Test-Link([string]$Path) { (Get-Item -LiteralPath $Path -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint }

$root = Join-Path $env:TEMP "oot-local-coop-test-$PID"
$kit = Join-Path $root 'kit'
$game = Join-Path $kit 'hookshot\game'
try {
    Write-File (Join-Path $kit 'coop-options.ini') "auto_rejoin = on`r`ncolor = red`r`n"
    Write-File (Join-Path $game 'longshot.exe') 'exe'
    Write-File (Join-Path $game 'hookshot_coop.dll') 'module'
    Write-File (Join-Path $game 'image.bin') 'image'
    Write-File (Join-Path $game 'longshot_options.ini') "fullscreen=1`r`nframe_rate_cap=2`r`ndiscord_status=1`r`nmouse_camera=1`r`nvsync=1`r`n"
    Write-File (Join-Path $game 'longshot_pipeline_states.bin') 'cache'
    Write-File (Join-Path $game 'coop.ini') "name = Saria`r`n"
    Write-File (Join-Path $game 'coop-saves.ini') 'solo'
    Write-File (Join-Path $game 'coop-character.txt') 'solo'
    Write-File (Join-Path $game 'player-key.txt') "p1key`r`n"
    Write-File (Join-Path $game 'romfs\a.txt') 'romfs'
    Write-File (Join-Path $game 'mods\m.txt') 'mod'
    Write-File (Join-Path $game 'user\save\host.sav') 'x'
    Write-File (Join-Path $game 'logs\longshot_log.txt') 'p1 log'

    # --- How many players ---
    $env:OOT_COOP_LOCAL_PLAYERS = '3'
    Check ((Read-KitLocalPlayers $kit) -eq 3) 'players: OOT_COOP_LOCAL_PLAYERS answers without asking'
    $env:OOT_COOP_LOCAL_PLAYERS = '7'
    $n = Read-KitLocalPlayers $kit
    Check ($n -ge 1 -and $n -le 4) "players: a number past 4 is not taken ($n)"
    Remove-Item env:OOT_COOP_LOCAL_PLAYERS
    Check ((Get-KitLocalPlayersLast $kit) -eq 1) 'players: 1 the first time'
    Write-File (Join-Path $kit 'hookshot\local-players.txt') "2`r`n"
    Check ((Get-KitLocalPlayersLast $kit) -eq 2) 'players: the last count is remembered'
    Write-File (Join-Path $kit 'hookshot\local-players.txt') "9`r`n"
    Check ((Get-KitLocalPlayersLast $kit) -eq 1) 'players: a damaged file is 1'
    Write-File (Join-Path $kit 'hookshot\local-players.txt') "2`r`n"
    if (-not (Test-KitLocalConsole)) {
        Check ((Read-KitLocalPlayers $kit) -eq 1) 'players: an unattended start (no console) is 1 player, not the remembered 2'
    }
    # Nothing waits: the choice is read from keys already pressed (a solo start is not slower).
    $t = [Diagnostics.Stopwatch]::StartNew()
    [void](Read-KitLocalPlayers $kit)
    Check ($t.ElapsedMilliseconds -lt 1000) "players: no wait ($($t.ElapsedMilliseconds) ms)"
    $lc = Read-File (Join-Path $scripts 'local-coop.ps1')
    Check (-not ($lc -match 'function Read-KitLocalPlayers\([^)]*Seconds') -and $lc.Contains('[Console]::KeyAvailable')) 'players: keys already pressed only, no time to wait'
    $st = Read-File (Join-Path $scripts 'start.ps1')
    Check ($st.Contains('Show-KitLocalPlayersHint')) 'Start Squadala says the count at its start, so a digit can be pressed while it goes on'

    # --- The instance folder ---
    $inst = Join-Path $game 'local-players\player2'
    $said = @(Sync-KitLocalInstance $game $inst)
    Check ($said.Count -eq 0) "instance: made without a problem ($($said -join ' | '))"
    Check ((Test-Link (Join-Path $inst 'romfs')) -and (Read-File (Join-Path $inst 'romfs\a.txt')) -eq 'romfs') 'instance: romfs is a junction to the game''s'
    Check ((Test-Link (Join-Path $inst 'mods'))) 'instance: mods is a junction'
    Check (-not (Test-Link (Join-Path $inst 'user')) -and -not (Test-Path (Join-Path $inst 'user\save\host.sav'))) 'instance: user is its own (no host save of player 1)'
    Check (-not (Test-Path (Join-Path $inst 'logs'))) 'instance: logs not linked (the game makes its own)'
    Check (-not (Test-Path (Join-Path $inst 'local-players'))) 'instance: local-players not linked into itself'
    Check ((Read-File (Join-Path $inst 'longshot.exe')) -eq 'exe' -and (Read-File (Join-Path $inst 'hookshot_coop.dll')) -eq 'module') 'instance: the exe and the module are there'
    Write-File (Join-Path $game 'image.bin') 'image changed in place'   # (a hard link shows the game's own bytes)
    Check ((Read-File (Join-Path $inst 'image.bin')) -eq 'image changed in place') 'instance: image.bin is a hard link, not a copy'
    foreach ($own in 'coop.ini', 'coop-saves.ini', 'coop-character.txt', 'player-key.txt') {
        Check (-not (Test-Path (Join-Path $inst $own)) -or (Read-File (Join-Path $inst $own)) -ne (Read-File (Join-Path $game $own))) "instance: $own is not player 1's"
    }
    Check ((Read-File (Join-Path $inst 'longshot_pipeline_states.bin')) -eq 'cache') 'instance: a file the game writes in place is copied'
    # A rebuild replaces the module: linked again at the next start.
    Remove-Item -LiteralPath (Join-Path $game 'hookshot_coop.dll')
    Write-File (Join-Path $game 'hookshot_coop.dll') 'module v2'
    [void](Sync-KitLocalInstance $game $inst)
    Check ((Read-File (Join-Path $inst 'hookshot_coop.dll')) -eq 'module v2') 'instance: a rebuilt module is linked again'
    # The copied cache is the instance's own after that.
    Write-File (Join-Path $inst 'longshot_pipeline_states.bin') 'mine'
    [void](Sync-KitLocalInstance $game $inst)
    Check ((Read-File (Join-Path $inst 'longshot_pipeline_states.bin')) -eq 'mine' -and (Read-File (Join-Path $game 'longshot_pipeline_states.bin')) -eq 'cache') 'instance: its own copy stays, the game''s untouched'

    # A junction that names another folder (the game moved, the kit copied): made again to this game's.
    $elsewhere = Join-Path $root 'old-game\romfs'
    Write-File (Join-Path $elsewhere 'a.txt') 'old place'
    [System.IO.Directory]::Delete((Join-Path $inst 'romfs'), $false)
    New-Item -ItemType Junction -Path (Join-Path $inst 'romfs') -Target $elsewhere | Out-Null
    $said = @(Sync-KitLocalInstance $game $inst)
    Check ((Read-File (Join-Path $inst 'romfs\a.txt')) -eq 'romfs' -and $said.Count -eq 0) "instance: a link to an old place is made again ($($said -join ' | '))"
    Check ((Read-File (Join-Path $elsewhere 'a.txt')) -eq 'old place') 'instance: the old place is left as it was'
    # A link whose folder is gone (the old game folder deleted): made again too.
    [System.IO.Directory]::Delete((Join-Path $inst 'mods'), $false)
    $gone = Join-Path $root 'gone-mods'
    New-Item -ItemType Directory -Path $gone | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $inst 'mods') -Target $gone | Out-Null
    [System.IO.Directory]::Delete($gone, $false)
    $said = @(Sync-KitLocalInstance $game $inst)
    Check ((Read-File (Join-Path $inst 'mods\m.txt')) -eq 'mod') "instance: a link to a folder that is gone is made again ($($said -join ' | '))"

    # --- Copies of this game folder never take the local players (review: robocopy follows the junctions) ---
    # hookshot-build.ps1's copy from another kit's game: its command as written, run on this game folder.
    $hb = Read-File (Join-Path $scripts 'hookshot-build.ps1')
    $line = @($hb -split "`r?`n" | Where-Object { $_ -match '^\s*& robocopy \$from \$Out /E /R:1' })
    Check ($line.Count -eq 1 -and $line[0].Contains('/XD @xdPlayers') -and $line[0].Contains('coop-player.ini')) 'kit build copy: the local and Nucleus players'' folders left out'
    Check ($hb -match "\`$xdPlayers = @\('local-players', 'nucleus-players'\)") 'kit build copy: both folders named'
    Check ($hb -match "\`$xd = @\([^)]*'local-players', 'nucleus-players'\)") 'kit build copy from the game cache: both folders left out too'
    $copy = Join-Path $root 'copied-game'
    $from = $game; $Out = $copy
    $xdPlayers = @('local-players', 'nucleus-players') | ForEach-Object { Join-Path $from $_ }
    & robocopy $from $Out /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD @xdPlayers /XF coop.ini coop-options.ini coop-player.ini *.log *_log.txt | Out-Null
    Check ($LASTEXITCODE -lt 8 -and (Test-Path (Join-Path $copy 'romfs\a.txt')) -and -not (Test-Path (Join-Path $copy 'local-players'))) "kit build copy: the game copied, local-players not (robocopy $LASTEXITCODE)"
    # start.ps1's move of the whole kit: no folder link followed, the players' own files kept.
    $st = Read-File (Join-Path $scripts 'start.ps1')
    Check ($st -match '& robocopy \$kit \$to /E /XJD') 'kit move: robocopy /XJD (no folder link followed)'
    $moved = Join-Path $root 'moved-kit'
    & robocopy $kit $moved /E /XJD /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    $mi = Join-Path $moved 'hookshot\game\local-players\player2'
    Check ($LASTEXITCODE -lt 8 -and (Test-Path (Join-Path $mi 'user')) -and -not (Test-Path (Join-Path $mi 'romfs')) -and (Test-Path (Join-Path $moved 'hookshot\game\romfs\a.txt'))) "kit move: the players' own folders, no copy of the linked game (robocopy $LASTEXITCODE)"

    # --- The player's files for the session ---
    Write-KitLocalPlayer $inst $kit 2 27561 'pw123' 30
    $coop = Read-File (Join-Path $inst 'coop.ini')
    Check ($coop -match "server = 127\.0\.0\.1`r`nport = 27561`r`nname = Player 2`r`npassword = pw123") "coop.ini: the session and 'Player 2' ($coop)"
    $mine = Read-File (Join-Path $inst 'coop-player.ini')
    Check ((Get-KitLocalIniValue $mine 'LICHII_COOP_PAD') -eq '2' -and (Get-KitLocalIniValue $mine 'LICHII_COOP_KEYBOARD') -eq 'off') "coop-player.ini: pad 2, no keyboard ($mine)"
    Check ((Get-KitLocalIniValue $mine 'auto_rejoin') -eq '' -and (Get-KitLocalIniValue $mine 'color') -eq '') 'coop-player.ini: nothing of the kit''s (player 1''s colour stays theirs)'
    Check ((Read-File (Join-Path $inst 'coop-kit.txt')).Trim() -eq $kit) 'coop-kit.txt names the kit'
    $key = (Read-File (Join-Path $inst 'player-key.txt')).Trim()
    Check ($key -match '^[0-9a-f]{32}$' -and $key -ne 'p1key') 'player-key.txt: a key of its own'
    $ls = Read-File (Join-Path $inst 'longshot_options.ini')
    foreach ($kv in 'fullscreen=0', 'discord_status=0', 'mouse_camera=0', 'frame_rate_cap=-1', 'vsync=1') {
        Check ($ls -match "(?m)^$kv\r?$") "longshot_options.ini: $kv"
    }
    Check ((Read-File (Join-Path $game 'longshot_options.ini')) -match 'fullscreen=1') 'longshot_options.ini: player 1''s own untouched'
    # What the player chose in the game stays: a name, a look, another controller.
    Write-File (Join-Path $inst 'coop.ini') "server = 127.0.0.1`r`nport = 1`r`nname = Mido`r`npassword = old`r`nlook_boots = black`r`ncolor = pink`r`n"
    Write-File (Join-Path $inst 'coop-player.ini') "# mine`r`nLICHII_COOP_PAD=4`r`nlook_hat = green`r`n"
    $key1 = $key
    Write-KitLocalPlayer $inst $kit 2 27562 'pw456' 60
    $coop = Read-File (Join-Path $inst 'coop.ini')
    Check ($coop -match 'name = Mido' -and $coop -match 'look_boots = black' -and $coop -match 'color = pink' -and $coop -match 'port = 27562' -and $coop -notmatch 'password = old') "coop.ini: the name and look kept, the session new ($coop)"
    $mine = Read-File (Join-Path $inst 'coop-player.ini')
    Check ((Get-KitLocalIniValue $mine 'LICHII_COOP_PAD') -eq '4' -and (Get-KitLocalIniValue $mine 'look_hat') -eq 'green') "coop-player.ini: the player's own controller and look kept ($mine)"
    Check ((Read-File (Join-Path $inst 'player-key.txt')).Trim() -eq $key1) 'player-key.txt: kept'
    Check ((Read-File (Join-Path $inst 'longshot_options.ini')) -match '(?m)^frame_rate_cap=0\r?$') 'longshot_options.ini: local_fps 60 is the console''s 60'

    # --- The frame cap ---
    Check ((Get-KitLocalFps $kit) -eq 30) 'local_fps: 30 by default'
    Write-File (Join-Path $kit 'coop-options.ini') "local_fps = 60`r`n"
    Check ((Get-KitLocalFps $kit) -eq 60) 'local_fps = 60'
    Write-File (Join-Path $kit 'coop-options.ini') "local_fps = 144`r`n"
    Check ((Get-KitLocalFps $kit) -eq 30) 'local_fps: anything else is 30'

    # --- Each game's variables ---
    $env:LICHII_COOP_PAD = 'auto'; $env:LICHII_COOP_COLOR = 'red'
    $script:KitLocalPlayer1Color = 'red'
    Set-KitLocalEnvironment 1 27561 'pw' 30
    Check ($env:OOT_COOP_LOCAL_HOST -eq '27561:pw' -and $env:LICHII_COOP_PAD -eq '1' -and $env:LICHII_COOP_KEYBOARD -eq 'focus') 'game 1: hosts, pad 1 and the keyboard'
    Check ($env:HOS_FULLSCREEN -eq '0' -and $env:HOS_FPS_MAX -eq '30' -and $env:LICHII_COOP_COLOR -eq 'red' -and -not $env:HOS_WINDOW_NOACTIVATE) 'game 1: a window, the cap, its colour, takes the focus'
    Set-KitLocalEnvironment 3 27561 'pw' 30
    Check (-not $env:OOT_COOP_LOCAL_HOST -and -not $env:LICHII_COOP_PAD -and -not $env:LICHII_COOP_KEYBOARD -and -not $env:LICHII_COOP_COLOR) 'game 3: no host, its controller and colour from its coop-player.ini (not player 1''s)'
    Check ($env:HOS_WINDOW_NOACTIVATE -eq '1' -and -not $env:HOS_FPS_MAX) 'game 3: opens behind (its cap is in its own options file)'
    foreach ($k in 'OOT_COOP_LOCAL_HOST', 'HOS_WINDOW_NOACTIVATE', 'HOS_FULLSCREEN', 'HOS_FPS_MAX', 'LICHII_COOP_PAD', 'LICHII_COOP_KEYBOARD', 'LICHII_COOP_COLOR') { Remove-Item "env:$k" -ErrorAction SilentlyContinue }

    # --- The session's port and password ---
    $p = Get-KitLocalPort
    Check ($p -ge 27560 -and $p -lt 27600) "port: a free one from 27560 ($p)"
    $hold = New-Object System.Net.Sockets.UdpClient (New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Loopback, $p))
    try { Check ((Get-KitLocalPort) -ne $p) 'port: one held is skipped' } finally { $hold.Close() }
    $pw = New-KitLocalPassword
    Check ($pw -match '^[0-9a-f]{24}$' -and $pw -ne (New-KitLocalPassword)) 'password: random, 24 letters'

    # --- The windows ---
    $two = Get-KitLocalRects @(0, 0, 1920, 1040) 2
    Check ($two.Count -eq 2 -and $two[0][0] -eq 0 -and $two[1][0] -eq 960 -and $two[0][2] -eq 960 -and $two[0][1] -eq $two[1][1]) "2 games side by side ($(($two | ForEach-Object { $_ -join ',' }) -join ' / '))"
    $four = Get-KitLocalRects @(-1920, 100, 0, 1180) 4
    Check ($four.Count -eq 4 -and $four[3][0] -eq -960 -and $four[3][1] -eq 640 -and $four[0][3] -eq 540) "4 games in quarters, on a monitor left of the primary ($(($four | ForEach-Object { $_ -join ',' }) -join ' / '))"
    $three = Get-KitLocalRects @(0, 0, 1920, 1040) 3
    Check ($three.Count -eq 3 -and $three[2][0] -eq 0 -and $three[2][1] -eq 520) '3 games: quarters, the 4th empty'
    foreach ($r in @($two + $four + $three)) { if ($r[2] -le 0 -or $r[3] -le 0) { Check $false "a window with no size: $($r -join ',')" } }

    # --- The start, the kit and collect-logs use it ---
    $play = Read-File (Join-Path $scripts 'play.ps1')
    Check ($play.Contains("'local-coop.ps1'") -and $play.Contains('Read-KitLocalPlayers') -and $play.Contains('Start-KitLocalCoop')) 'play.ps1 asks and starts local co-op'
    Check ($play -match "Mode -eq 'title' -and -not \`$env:OOT_COOP_TEST_KIT") 'play.ps1 asks only at the kit''s own start, never in the autotest''s kit mode'
    Check ((Read-File (Join-Path $release 'make_kit_hookshot.ps1')).Contains("'local-coop.ps1'")) 'the kit ships local-coop.ps1'
    Check ((Read-File (Join-Path $scripts 'collect-logs.ps1')).Contains("local-players")) 'collect-logs takes the local players'' logs'
    Check ((Read-File (Join-Path $release 'kit\coop-options.ini')) -match '(?m)^#\s*local_fps') 'coop-options.ini explains local_fps'
} finally {
    if (Test-Path -LiteralPath $root) {
        # (the junctions first: removing a junction leaves what it names)
        Get-ChildItem -LiteralPath (Join-Path $game 'local-players') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            Get-ChildItem -LiteralPath $_.FullName -Directory -Force | Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint } |
                ForEach-Object { [System.IO.Directory]::Delete($_.FullName, $false) }
        }
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
