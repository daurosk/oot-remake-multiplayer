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
    # C2-2 (owner, 0.12.0): a key never starts more games by itself; only the question's answer does, and only the answer
    # is written. A solo start asks nothing and waits for nothing.
    $lpFile = Join-Path $kit 'hookshot\local-players.txt'
    $key = { param($c, $k) [pscustomobject]@{ KeyChar = $c; Key = $k } }
    $two = { & $key '2' 'D2' }.GetNewClosure()
    $none = { $null }
    Remove-Item -LiteralPath $lpFile -Force
    $t = [Diagnostics.Stopwatch]::StartNew()
    Check ((Read-KitLocalPlayers $kit 15 $two @()) -eq 1) 'players: a solo start (nothing pressed, no last answer) is 1, the question is not asked'
    Check ($t.ElapsedMilliseconds -lt 1000) "players: a solo start waits for nothing ($($t.ElapsedMilliseconds) ms)"
    Check (-not (Test-Path -LiteralPath $lpFile)) 'players: a solo start writes nothing'
    Check ((Read-KitLocalPlayers $kit 1 $none @('2')) -eq 1) 'players: a stray 2 pressed during the start and no answer -> 1 game'
    Check (-not (Test-Path -LiteralPath $lpFile)) 'players: a stray digit with no answer is never saved'
    Check ((Read-KitLocalPlayers $kit 1 $two @('x', '1')) -eq 1) 'players: other keys or 1 pressed during the start do not ask'
    $three = { & $key '3' 'D3' }.GetNewClosure()
    Check ((Read-KitLocalPlayers $kit 5 $three @('3')) -eq 3) 'players: 3 pressed, then 3 answered -> 3 games'
    Check ((Read-File $lpFile).Trim() -eq '3') 'players: the answer is written'
    Check ((Read-KitLocalPlayers $kit 1 $none @()) -eq 1) 'players: last answer 3, no answer this start -> 1 game (a start with no answer is 1, always)'
    Check ((Read-File $lpFile).Trim() -eq '3') 'players: no answer -> the file is not written'
    $enter = { & $key ([char]13) 'Enter' }.GetNewClosure()
    Check ((Read-KitLocalPlayers $kit 5 $enter @()) -eq 1) 'players: last answer 3, Enter -> 1 game'
    Check ((Read-File $lpFile).Trim() -eq '1') 'players: Enter is an answer of 1, written'
    Check ((Read-KitLocalPlayers $kit 5 $two @()) -eq 1) 'players: last answer 1 -> not asked again (solo)'
    $nKey = { & $key 'n' 'N' }.GetNewClosure()
    Write-File $lpFile "2`r`n"
    Check ((Read-KitLocalPlayers $kit 5 $nKey @()) -eq 1) 'players: N -> 1 game'
    $env:OOT_COOP_LOCAL_PLAYERS = '2'
    Check ((Read-KitLocalPlayers $kit 1 $none @()) -eq 2) 'players: OOT_COOP_LOCAL_PLAYERS still answers outright'
    Remove-Item env:OOT_COOP_LOCAL_PLAYERS
    $lc = Read-File (Join-Path $scripts 'local-coop.ps1')
    Check ($lc.Contains('[Console]::KeyAvailable') -and $lc -match 'function Read-KitLocalPlayersAnswer') 'players: keys pressed during the start only make it ask (Read-KitLocalPlayersAnswer)'
    $st = Read-File (Join-Path $scripts 'start.ps1')
    Check ($st.Contains('Show-KitLocalPlayersHint')) 'Start Squadala says at its start how to ask for local co-op'

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

    # --- What player 2's game will see through its links (run local-coop-count-run: games 2 and 3 started with an
    # empty mods folder and stopped at their start) ---
    $blind = @(Test-KitLocalLinks $game $inst)
    Check ($blind.Count -eq 0) "links: every linked folder shows the game's ($($blind -join ' | '))"
    [System.IO.Directory]::Delete((Join-Path $inst 'mods'), $false)
    $empty = Join-Path $root 'empty-mods'
    New-Item -ItemType Directory -Path $empty | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $inst 'mods') -Target $empty | Out-Null
    $blind = @(Test-KitLocalLinks $game $inst)
    Check ($blind.Count -eq 1 -and $blind[0] -match 'mods' -and $blind[0] -match 'shows 0 of the 1') "links: a mods link that shows nothing is said ($($blind -join ' | '))"
    [System.IO.Directory]::Delete((Join-Path $inst 'mods'), $false)
    [void](Sync-KitLocalInstance $game $inst)
    Check (@(Test-KitLocalLinks $game $inst).Count -eq 0) 'links: made again, all shown'

    # --- A folder of the instance's own, made before the game had one of that name (review: it stopped every start) ---
    Write-File (Join-Path $inst 'screenshots\mine.png') 'mine'
    Write-File (Join-Path $game 'screenshots\p1.png') 'p1'
    $said = @(Sync-KitLocalInstance $game $inst)
    Check ($said.Count -eq 1 -and $said[0] -match 'screenshots is a folder of its own') "own folder: kept and said ($($said -join ' | '))"
    Check ((Read-File (Join-Path $inst 'screenshots\mine.png')) -eq 'mine' -and -not (Test-Path (Join-Path $game 'screenshots\mine.png'))) 'own folder: its file is kept where it was'
    $blind = @(Test-KitLocalLinks $game $inst)
    Check ($blind.Count -eq 0) "own folder: one a start does not need stops nothing ($($blind -join ' | '))"
    # An empty one has nothing to keep: linked.
    New-Item -ItemType Directory -Path (Join-Path $inst 'texts') | Out-Null
    Write-File (Join-Path $game 'texts\t.txt') 'text'
    [void](Sync-KitLocalInstance $game $inst)
    Check ((Test-Link (Join-Path $inst 'texts')) -and (Read-File (Join-Path $inst 'texts\t.txt')) -eq 'text') 'own folder: an empty one becomes the link'
    # mods: a start needs it. Its own without the game's mods is said (and what to do); with them and more, it starts.
    [System.IO.Directory]::Delete((Join-Path $inst 'mods'), $false)
    Write-File (Join-Path $inst 'mods\other.txt') 'other'
    [void](Sync-KitLocalInstance $game $inst)
    $blind = @(Test-KitLocalLinks $game $inst)
    Check ($blind.Count -eq 1 -and $blind[0] -match 'shows 0 of the 1' -and $blind[0] -match 'folder of its own: move it') "own folder: mods without the game's mods is said ($($blind -join ' | '))"
    Write-File (Join-Path $inst 'mods\m.txt') 'mod'
    $blind = @(Test-KitLocalLinks $game $inst)
    Check ($blind.Count -eq 0) "own folder: mods with the game's and more starts ($($blind -join ' | '))"
    Remove-Item -LiteralPath (Join-Path $inst 'mods') -Recurse -Force
    # A link that is missing is still said, for any folder.
    [System.IO.Directory]::Delete((Join-Path $inst 'texts'), $false)
    $blind = @(Test-KitLocalLinks $game $inst)
    Check ($blind.Count -eq 2 -and ($blind -join ' ') -match 'mods' -and ($blind -join ' ') -match 'texts') "links: missing ones are said ($($blind -join ' | '))"
    [void](Sync-KitLocalInstance $game $inst)
    Check (@(Test-KitLocalLinks $game $inst).Count -eq 0) 'links: the missing ones are made, all shown'
    foreach ($n in 'screenshots', 'texts') {
        $l = Join-Path $inst $n
        if (Test-Link $l) { [System.IO.Directory]::Delete($l, $false) } else { Remove-Item -LiteralPath $l -Recurse -Force }
        Remove-Item -LiteralPath (Join-Path $game $n) -Recurse -Force
    }

    # --- Which player a running game is (its exe's folder); the pattern parses (it ended in one \ before) ---
    $lp = (Join-Path $game 'local-players') + '\'
    Check ((Get-KitLocalPlayerOfPath $lp (Join-Path $lp 'player3\longshot.exe')) -eq 3) 'running: local-players\player3\longshot.exe is player 3'
    Check ((Get-KitLocalPlayerOfPath $lp (Join-Path $lp 'PLAYER2\longshot.exe')) -eq 2) 'running: the folder''s case does not matter'
    Check ((Get-KitLocalPlayerOfPath $lp (Join-Path $lp 'player1\longshot.exe')) -eq 0) 'running: player1 is not one of the games it starts'
    Check ((Get-KitLocalPlayerOfPath $lp (Join-Path $lp 'player2x\longshot.exe')) -eq 0) 'running: player2x is not player 2'
    Check ((Get-KitLocalPlayerOfPath $lp (Join-Path $game 'longshot.exe')) -eq 0) 'running: game 1 is not a local player'
    Check ((Get-KitLocalPlayerOfPath $lp '') -eq 0) 'running: a process without a path'

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
    Write-KitLocalPlayer $inst $kit 2 27561 'pw123' (Get-KitLocalFps $kit)
    $coop = Read-File (Join-Path $inst 'coop.ini')
    Check ($coop -match "server = 127\.0\.0\.1`r`nport = 27561`r`nname = Player 2`r`npassword = pw123") "coop.ini: the session and 'Player 2' ($coop)"
    $mine = Read-File (Join-Path $inst 'coop-player.ini')
    Check ((Get-KitLocalIniValue $mine 'LICHII_COOP_PAD') -eq '2' -and (Get-KitLocalIniValue $mine 'LICHII_COOP_KEYBOARD') -eq 'off') "coop-player.ini: pad 2, no keyboard ($mine)"
    Check ((Get-KitLocalIniValue $mine 'auto_rejoin') -eq '' -and (Get-KitLocalIniValue $mine 'color') -eq '') 'coop-player.ini: nothing of the kit''s (player 1''s colour stays theirs)'
    Check ((Read-File (Join-Path $inst 'coop-kit.txt')).Trim() -eq $kit) 'coop-kit.txt names the kit'
    $key = (Read-File (Join-Path $inst 'player-key.txt')).Trim()
    Check ($key -match '^[0-9a-f]{32}$' -and $key -ne 'p1key') 'player-key.txt: a key of its own'
    $ls = Read-File (Join-Path $inst 'longshot_options.ini')
    foreach ($kv in 'fullscreen=0', 'discord_status=0', 'mouse_camera=0', 'frame_rate_cap=2', 'vsync=1') {
        Check ($ls -match "(?m)^$kv\r?$") "longshot_options.ini: $kv (no local_fps: the player's own frame cap, uncapped as 0.11.12)"
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
    Write-KitLocalPlayer $inst $kit 2 27562 'pw456' 30
    Check ((Read-File (Join-Path $inst 'longshot_options.ini')) -match '(?m)^frame_rate_cap=-1\r?$') 'longshot_options.ini: local_fps 30 is Longshot''s 30 FPS cap'

    # --- The frame cap ---
    Check ((Get-KitLocalFps $kit) -eq 0) 'local_fps: no cap of ours by default (uncapped, as 0.11.12)'
    Write-File (Join-Path $kit 'coop-options.ini') "# local_fps = 30`r`n"
    Check ((Get-KitLocalFps $kit) -eq 0) 'local_fps: the shipped commented line is no cap'
    Write-File (Join-Path $kit 'coop-options.ini') "local_fps = 30`r`n"
    Check ((Get-KitLocalFps $kit) -eq 30) 'local_fps = 30'
    Write-File (Join-Path $kit 'coop-options.ini') "local_fps = 60`r`n"
    Check ((Get-KitLocalFps $kit) -eq 60) 'local_fps = 60'
    Write-File (Join-Path $kit 'coop-options.ini') "local_fps = 144`r`n"
    Check ((Get-KitLocalFps $kit) -eq 0) 'local_fps: anything else is no cap'

    # --- Each game's variables ---
    $env:LICHII_COOP_PAD = 'auto'; $env:LICHII_COOP_COLOR = 'red'
    $script:KitLocalPlayer1Color = 'red'
    Set-KitLocalEnvironment 1 27561 'pw' 30
    Check ($env:OOT_COOP_LOCAL_HOST -eq '27561:pw' -and $env:LICHII_COOP_PAD -eq '1' -and $env:LICHII_COOP_KEYBOARD -eq 'focus') 'game 1: hosts, pad 1 and the keyboard'
    Check ($env:HOS_FULLSCREEN -eq '0' -and $env:HOS_FPS_MAX -eq '30' -and $env:LICHII_COOP_COLOR -eq 'red' -and -not $env:HOS_WINDOW_NOACTIVATE) 'game 1: a window, the cap, its colour, takes the focus'
    Set-KitLocalEnvironment 1 27561 'pw' 0
    Check (-not $env:HOS_FPS_MAX -and $env:HOS_FULLSCREEN -eq '0') 'game 1 with no local_fps: no cap of ours (the player''s own)'
    Set-KitLocalEnvironment 3 27561 'pw' 30
    Check (-not $env:OOT_COOP_LOCAL_HOST -and -not $env:LICHII_COOP_PAD -and -not $env:LICHII_COOP_KEYBOARD -and -not $env:LICHII_COOP_COLOR) 'game 3: no host, its controller and colour from its coop-player.ini (not player 1''s)'
    Check ($env:HOS_WINDOW_NOACTIVATE -eq '1' -and -not $env:HOS_FPS_MAX) 'game 3: opens behind (its cap is in its own options file)'
    # (row 301: each game's window shape, Hookshot patch 0047's switches; none given: none set, none left from before)
    Check (-not $env:HOS_ASPECT -and -not $env:HOS_OUTPUT_RES -and -not $env:HOS_WINDOW_SIZE) 'game 3 with no shape: the game''s own picture'
    Set-KitLocalEnvironment 2 27561 'pw' 0 @{ HOS_ASPECT = '1904:2048'; HOS_OUTPUT_RES = '1904x2048'; HOS_WINDOW_SIZE = '1269x1365' }
    Check ($env:HOS_ASPECT -eq '1904:2048' -and $env:HOS_OUTPUT_RES -eq '1904x2048' -and $env:HOS_WINDOW_SIZE -eq '1269x1365' -and $env:HOS_WINDOW_NOACTIVATE -eq '1') 'game 2: starts with its window''s shape and size'
    Set-KitLocalEnvironment 3 27561 'pw' 0
    Check (-not $env:HOS_ASPECT -and -not $env:HOS_OUTPUT_RES -and -not $env:HOS_WINDOW_SIZE) 'the next game: the shape of the one before is not kept'
    foreach ($k in $script:KitLocalRunKeys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    $lcText = Read-File (Join-Path $scripts 'local-coop.ps1')
    Check (-not ($lcText -match "KitLocalKeys \+ @\('OOT_COOP_LOCAL_HOST'.*\{ \`$saved")) 'the starts put back every variable of KitLocalRunKeys (one list)'
    foreach ($k in 'HOS_ASPECT', 'HOS_OUTPUT_RES', 'HOS_WINDOW_SIZE', 'HOS_FULLSCREEN', 'OOT_COOP_LOCAL_HOST') { Check ($script:KitLocalRunKeys -contains $k) "the starts put back $k" }

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
    Check ($two[0][1] -eq 0 -and $two[0][3] -eq 1040 -and $two[1][3] -eq 1040) 'row 301: 2 games each fill their half, the whole height (no 16:9 band in the middle)'
    # (the owner's 4K screen at 150 %: the work area in physical pixels, 3840 x 2100 with a 60-pixel taskbar)
    $k4 = Get-KitLocalRects @(0, 0, 3840, 2100) 2
    Check ($k4[0][2] -eq 1920 -and $k4[0][3] -eq 2100 -and $k4[1][0] -eq 1920) "row 301: 4K, 2 games: two 1920 x 2100 halves ($(($k4 | ForEach-Object { $_ -join ',' }) -join ' / '))"
    Initialize-KitLocalWin32
    $f144 = [KitLocalWin32]::Frame(144)   # (WS_OVERLAPPEDWINDOW's frame at 144 dpi, from Windows: AdjustWindowRectExForDpi)
    $fw = $f144[2] - $f144[0]; $fh = $f144[3] - $f144[1]
    $c4 = Get-KitLocalClient $k4[0] $f144
    Check ($c4[0] -eq 1920 - $fw -and $c4[1] -eq 2100 - $fh) "row 301: the half's picture area is the window less its frame ($($c4 -join 'x'), frame $($f144 -join ','))"
    $sh = Get-KitLocalShapeEnv $c4 144
    $ew = [int]([math]::Floor($c4[0] / 8) * 8); $eh = [int]([math]::Floor($c4[1] / 8) * 8)
    Check ($sh.HOS_ASPECT -eq "$($c4[0]):$($c4[1])" -and $sh.HOS_OUTPUT_RES -eq "${ew}x${eh}" -and $sh.HOS_WINDOW_SIZE -eq "$([int][math]::Round($c4[0] * 96 / 144))x$([int][math]::Round($c4[1] * 96 / 144))") "row 301: game switches for that half: shape, size (8-pixel steps), the window at 96 dpi ($($sh.HOS_ASPECT) $($sh.HOS_OUTPUT_RES) $($sh.HOS_WINDOW_SIZE))"
    Check ($c4[0] / $c4[1] -lt 1.0 -and $c4[0] / $c4[1] -gt 0.85) "row 301: a 4K half is about 8:9, narrower than 5:4 (Hookshot patch 0047 takes it: $([math]::Round($c4[0] / $c4[1], 4)))"
    $own = Get-KitLocalShapeEnv $c4 144 1080
    $ow, $oh = $own.HOS_OUTPUT_RES -split 'x'
    Check ($own.HOS_ASPECT -eq $sh.HOS_ASPECT -and [int]$oh -eq 1080 -and [math]::Abs([int]$ow / 1080 - $c4[0] / $c4[1]) -lt 0.01) "row 301: a player's own lower output_res (1080) keeps its height, at the window's shape ($($own.HOS_OUTPUT_RES))"
    $big = Get-KitLocalShapeEnv @(3824, 4200) 96
    $bw, $bh = $big.HOS_OUTPUT_RES -split 'x'
    Check ([int]$bw * [int]$bh -le 5120 * 2880 -and [math]::Abs([int]$bw / [int]$bh - 3824 / 4200) -lt 0.01) "row 301: a half of an 8K screen: at most 5120 x 2880's pixels, its shape kept ($($big.HOS_OUTPUT_RES))"
    Check ((Get-KitLocalClient @(0, 0, 300, 300) $f144)[0] -eq 0 -and (Get-KitLocalShapeEnv @(0, 0) 96).Count -eq 0) 'row 301: a window too small: no switches (the game''s own picture)'
    $qc = Get-KitLocalClient (Get-KitLocalRects @(0, 0, 3840, 2100) 4)[3] $f144
    $q = Get-KitLocalShapeEnv $qc 144
    Check ($qc[0] -eq 1920 - $fw -and $qc[1] -eq 1050 - $fh -and $q.HOS_ASPECT -eq "$($qc[0]):$($qc[1])") "row 301: 4K, a quarter: its own shape ($($q.HOS_ASPECT))"
    Write-File (Join-Path $root 'opts\longshot_options.ini') "output_res=1920x1080`r`nfullscreen=0`r`n"
    Check ((Get-KitLocalOutputHeight (Join-Path $root 'opts')) -eq 1080) 'row 301: the player''s own output_res height is read'
    Write-File (Join-Path $root 'opts\longshot_options.ini') "output_res=2`r`n"
    Check ((Get-KitLocalOutputHeight (Join-Path $root 'opts')) -eq 2160) 'row 301: an output_res number saved before (2: 3840 x 2160)'
    Check ((Get-KitLocalOutputHeight (Join-Path $root 'none')) -eq 0) 'row 301: no options file: the game''s own'
    Initialize-KitLocalWin32
    $dpi = [KitLocalWin32]::Dpi(); $fr = [KitLocalWin32]::Frame($dpi); $wa = [KitLocalWin32]::WorkArea()
    Check ($dpi -ge 96 -and $fr[0] -lt 0 -and $fr[1] -lt $fr[0] -and $fr[2] -gt 0) "row 301: this PC's DPI ($dpi) and the window frame there ($($fr -join ','))"
    Check ($wa[2] -gt $wa[0] -and $wa[3] -gt $wa[1]) "row 301: the work area in physical pixels ($($wa -join ','))"
    $f96 = [KitLocalWin32]::Frame(96); $f2 = [KitLocalWin32]::Frame(192)
    Check (($f2[2] - $f2[0]) -gt ($f96[2] - $f96[0]) -and ($f2[3] - $f2[1]) -gt ($f96[3] - $f96[1])) "row 301: the frame grows with the DPI ($($f96 -join ',') at 96, $($f2 -join ',') at 192)"
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

    # From the game (local-coop-start.ps1, the Local co-op page's Start): game 1 hosts already; the missing players'
    # games are made and started into its session. This made-up longshot.exe does not start, so each is a game that
    # did not start (the page's "Did not start: player 2 and 3."), and nothing is left half done.
    Check ((Read-File (Join-Path $release 'make_kit_hookshot.ps1')).Contains("'local-coop-start.ps1'")) 'the kit ships local-coop-start.ps1'
    Check (@(Get-KitLocalRunning $game).Count -eq 0) 'from the game: no extra game runs in the made-up folder'
    $result = Join-Path $game 'local-players\start-result.txt'
    $env:LICHII_COOP_PAD = '1'   # (game 1's, inherited by the launcher: never player 2's)
    $null = @(Add-KitLocalCoop $kit (Join-Path $game 'longshot.exe') 3 27561 'pw-from-game' 0 $result 6>$null)
    $said = Read-File $result
    Check ($said -match '(?m)^started=\r?$') "from the game: nothing started ($said)"
    Check ($said -match '(?m)^failed=2,3\r?$') 'from the game: players 2 and 3 did not start (the result says so)'
    Check ($said -match '(?m)^problem=Player 2') 'from the game: the problem line names the first game that did not start'
    $p3 = Read-File (Join-Path $game 'local-players\player3\coop.ini')
    Check ($p3 -match '(?m)^server = 127\.0\.0\.1' -and $p3 -match '(?m)^port = 27561' -and $p3 -match '(?m)^password = pw-from-game') 'from the game: player 3 gets game 1''s session'
    Check ((Read-File (Join-Path $game 'local-players\player3\coop-player.ini')) -match '(?m)^LICHII_COOP_PAD\s*=\s*3') 'from the game: player 3 reads controller 3'
    Check ("$env:LICHII_COOP_PAD" -eq '1') 'from the game: the launcher''s own variables are put back'
    Check ((Read-File (Join-Path $game 'local-players\instances.txt')) -match '(?m)^3=') 'from the game: instances.txt lists the session''s games for collect-logs'
    $null = @(Add-KitLocalCoop $kit (Join-Path $game 'longshot.exe') 9 27561 'pw-from-game' 0 $result 6>$null)
    Check ((Read-File $result) -match '(?m)^failed=2,3,4\r?$') 'from the game: more than 4 asked = 4 games (the most)'
    $start = Read-File (Join-Path $scripts 'local-coop-start.ps1')
    Check ($start.Contains('Add-KitLocalCoop') -and $start.Contains('problem-send.ps1')) 'local-coop-start.ps1 runs Add-KitLocalCoop and watches each game for problem reports'
    Remove-Item -Path env:LICHII_COOP_PAD -ErrorAction SilentlyContinue
    Initialize-KitLocalWin32
    Check (-not [KitLocalWin32]::FillsScreen([IntPtr]::Zero)) 'from the game: no window is not a full-screen one (game 1''s window is placed only when not full screen)'
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
