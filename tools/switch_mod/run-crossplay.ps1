# SPDX-License-Identifier: GPL-3.0-or-later
# ExeFS cross-play run (unattended): the ExeFS module (switch/proto + switch/net, tools/switch_mod/build.py) in a stock
# Fritchii joins ONE coop_server session with the two Longshot games of test lane 1, and each side sees the other.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\switch_mod\run-crossplay.ps1 -Expect <sha of this checkout>
#       [-Out <folder>] [-Emu <Fritchii folder>] [-Game <.nup>] [-From <kit emulator folder: keys, firmware, config>]
#       [-SetupOnly]   (only the setup proof)
#
# Exit 0 = PASS, 1 = FAIL, 2 = SETUP-FAIL. Every line goes to <Out>\result.txt; logs and shots stay in <Out>.
# Order: setup proof (seconds, nothing started) -> module build for the lane's protocol -> lane 1's two Longshot games
# and the lane's own coop_server through tools\autotest\hookshot.py run --lane 1 (scenario exefs-crossplay: both stand
# still 8 minutes) -> once the server logged both Longshot players joined, Fritchii with the module -> judge.
# The Fritchii starts only after both Longshot games are in, so the module is never the session's first player
# (player 1 hosts the save) and it joins exactly once (coop_join.cpp does not retry a closed connection).
#
# Every value's source (nothing typed by hand):
#  - lane 1's game build: coop-kit\ls\lane1-A\lane-server.txt (written by offline\lane-game-for.ps1): commit, protocol,
#    server, dll_sha256; lane1-B's must be the same file, and both hookshot_coop.dll must hash to dll_sha256.
#  - the module's protocol: build.py --protocol-from <that commit> (kProtocolVersion in messages.hpp at the commit),
#    then checked equal to lane-server.txt's protocol and to the COOP_PROTOCOL_VERSION build.py wrote.
#  - lane 1's port 27520 and run lock: tools\autotest\lanes.py (PORT_BASE 27510 + 10 * lane).
#  - the server's lines: server\host_service.cpp "listening on port N", server\session.cpp
#    "player N joined: NICK (client: ...)"; hookshot.py writes the server's output to <run>\server-out.txt.
#  - the Longshot games' nicknames A and B: hookshot.py (LICHII_COOP_NAME = the game's letter); their line
#    "coop: player N 'NICK' joined": lichii\hook\coop_hook.cpp.
#  - the module's lines: switch\proto\main.cpp ("coop-proto: hello from subsdk1", "hook 0: frame hook in",
#    "game value: scene"), switch\net\coop_join.cpp ("coop-join: WELCOME: I am player N", "in the session: player N
#    'NICK'", "PLAYER_JOINED player N 'NICK'", "REFUSED", "FAIL", "disconnected"); svcOutputDebugString lands in the
#    Fritchii log at Info (fritchii-src svc_debug_string.cpp LOG_INFO(Debug_Emulated)).
#  - Fritchii's user folder set-up and the kit-path rewrite: wt-exefs-spike\spike\exefs-mod\run-fritchii.ps1
#    (1d1aafbd0, the preflight-b1 19:18 fix); the module slot: docs\exefs.md (subsdk1 is free in this game).
#  - the monitor: coord-multiplayer\offline\lib\test-display.ps1 (Get-TestDisplay), placement.py for the window.
# The join password is a test-only one, made fresh per run; it is passed to the server in the environment.
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$Expect,
    [string]$Out = '',
    [string]$Emu = '',
    [string]$Game = '',
    [string]$From = '',
    [int]$Lane = 1,
    # stop after the setup proof (nothing built, nothing started): exit 0 = the run could start now
    [switch]$SetupOnly
)
$ErrorActionPreference = 'Continue'
# (defaults after param(): an advanced script sees an empty $PSScriptRoot in param() defaults, reports/exefs-spike.md)
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Split-Path -Parent $repo
if (-not $Out) { $Out = Join-Path $root 'coord-multiplayer\offline\exefs-crossplay' }
if (-not $Emu) { $Emu = Join-Path $root 'exefs-spike\fritchii-v6.5\Fritchii-v6.5' }
if (-not $Game) { $Game = Join-Path $root 'The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup' }
if (-not $From) { $From = Join-Path $root 'coop-kit\auto-A\emulator' }
$py = Join-Path $root 'tools-venv\Scripts\python.exe'
$kitLs = Join-Path $root 'coop-kit\ls'
$port = 27510 + 10 * $Lane
$title = '0400486024DEC000'

New-Item -ItemType Directory -Force $Out | Out-Null
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$run = Join-Path $Out $stamp
New-Item -ItemType Directory -Force $run | Out-Null
$result = Join-Path $Out 'result.txt'
"# exefs-crossplay $stamp UTC, run folder $run" | Out-File -Encoding utf8 $result
function Res([string]$t) { $l = "$((Get-Date).ToUniversalTime().ToString('HH:mm:ss')) $t"; Write-Host $l; $l | Out-File -Append -Encoding utf8 $result }
function SetupFail([string]$t) { Res "SETUP-FAIL $t"; Res 'RESULT: SETUP-FAIL'; exit 2 }

# --- SETUP PROOF (seconds; nothing is started, nothing written outside -Out)
$sha = (git -C $repo rev-parse HEAD).Trim()
if (-not $sha.ToLower().StartsWith($Expect.ToLower())) { SetupFail "checkout $repo is at $sha, not $Expect" }
$dirty = @(git -C $repo status --porcelain --untracked-files=no)
if ($dirty.Count) { SetupFail "checkout $repo has uncommitted changes ($($dirty[0]))" }
Res "checkout $repo at $sha"
foreach ($p in @($py, (Join-Path $Emu 'fritchii.exe'), $Game, (Join-Path $repo 'tools\switch_mod\build.py'),
                 (Join-Path $repo 'tools\autotest\hookshot.py'), (Join-Path $repo 'tools\autotest\lanes.py'),
                 (Join-Path $repo 'tools\autotest\placement.py'), (Join-Path $repo 'tools\autotest\scenarios\load.txt'),
                 (Join-Path $repo 'tools\autotest\scenarios\exefs-crossplay.A.txt'),
                 (Join-Path $repo 'tools\autotest\scenarios\exefs-crossplay.B.txt'),
                 (Join-Path $root 'coord-multiplayer\offline\lib\test-display.ps1'))) {
    if (-not (Test-Path -LiteralPath $p)) { SetupFail "missing $p" }
}
foreach ($d in 'keys', 'config', 'nand\system') {
    if (-not (Test-Path -LiteralPath (Join-Path $From "user\$d"))) { SetupFail "missing $(Join-Path $From "user\$d") (keys/firmware source)" }
}
# lane 1's game build, from its own record
$ls = @{}
foreach ($x in 'A', 'B') {
    $f = Join-Path $kitLs "lane$Lane-$x\lane-server.txt"
    if (-not (Test-Path -LiteralPath $f)) { SetupFail "missing $f (lane game not made by lane-game-for.ps1)" }
    $ls[$x] = @{}
    foreach ($line in Get-Content -LiteralPath $f) { if ($line -match '^(\w+)=(.*)$') { $ls[$x][$Matches[1]] = $Matches[2].Trim() } }
    foreach ($k in 'commit', 'protocol', 'server', 'dll_sha256') { if (-not $ls[$x][$k]) { SetupFail "$f has no $k=" } }
    $dll = Join-Path $kitLs "lane$Lane-$x\hookshot_coop.dll"
    if (-not (Test-Path -LiteralPath $dll)) { SetupFail "missing $dll" }
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash
    if ($h -ne $ls[$x]['dll_sha256'].ToUpper()) { SetupFail "lane$Lane-$x hookshot_coop.dll is $h, lane-server.txt says $($ls[$x]['dll_sha256']) (another run's module left in the lane?)" }
}
foreach ($k in 'commit', 'protocol', 'server', 'dll_sha256') {
    if ($ls['A'][$k] -ne $ls['B'][$k]) { SetupFail "lane$Lane-A and -B lane-server.txt differ in $k ($($ls['A'][$k]) / $($ls['B'][$k]))" }
}
$laneCommit = $ls['A']['commit']; $laneProto = [int]$ls['A']['protocol']; $server = $ls['A']['server']
if (-not (Test-Path -LiteralPath $server)) { SetupFail "lane server $server missing" }
if ((git -C $repo cat-file -t $laneCommit 2>$null) -ne 'commit') { SetupFail "lane commit $laneCommit is not in $repo (git fetch)" }
$hpp = (git -C $repo show "${laneCommit}:coop/protocol/include/coop/protocol/messages.hpp") -join "`n"
if ($hpp -notmatch 'kProtocolVersion\s*=\s*(\d+)' -or [int]$Matches[1] -ne $laneProto) { SetupFail "messages.hpp at $laneCommit does not say protocol $laneProto" }
Res "lane $Lane game: commit $laneCommit protocol $laneProto server $server dll $($ls['A']['dll_sha256'].Substring(0, 16))"
& $py -I (Join-Path $repo 'tools\autotest\hookshot.py') check (Join-Path $repo 'tools\autotest\scenarios\exefs-crossplay.A.txt') (Join-Path $repo 'tools\autotest\scenarios\exefs-crossplay.B.txt') *>> (Join-Path $run 'setup.log')
if ($LASTEXITCODE -ne 0) { SetupFail 'hookshot.py check of exefs-crossplay.A/B.txt did not parse' }
$lock = (& $py -I (Join-Path $repo 'tools\autotest\lanes.py') lock status --lane $Lane 2>&1) -join ' '
if ($lock -notmatch "lane ${Lane}: free") { SetupFail "lane $Lane not free: $lock" }
foreach ($n in 'longshot', 'fritchii', 'lichii') {
    if (Get-Process $n -ErrorAction SilentlyContinue) { SetupFail "a $n process runs already (the PC is not free for this run)" }
}
if (Get-NetUDPEndpoint -LocalPort $port -ErrorAction SilentlyContinue) { SetupFail "UDP port $port is taken already" }
. (Join-Path $root 'coord-multiplayer\offline\lib\test-display.ps1')
try { $td = Get-TestDisplay } catch { SetupFail ("$_" -replace '^SETUP-FAIL ', '') }
Res "display $($td.Name) ($($td.Reason))"
$shotLib = Join-Path $root 'coord-multiplayer\offline\lib\screen-shot.ps1'
if (-not (Test-Path -LiteralPath $shotLib)) { SetupFail "missing $shotLib (the shared window-shot helper)" }
. $shotLib
Res 'setup proof OK'
if ($SetupOnly) { Res 'RESULT: SETUP-OK (-SetupOnly: nothing built or started)'; exit 0 }
foreach ($l in (Clear-TestScreen -Bounds $td.Bounds)) { Res "test screen $($td.Name): $l" }   # (other windows minimised, none closed)

# --- the module, for the lane's protocol (a fresh test-only password per run)
$password = 'exefs-xplay-' + ([guid]::NewGuid().ToString('N').Substring(0, 12))
$mod = Join-Path $run 'module'
& $py (Join-Path $repo 'tools\switch_mod\build.py') --out $mod --protocol-from $laneCommit --join "127.0.0.1:$port" --password $password --name emulator *> (Join-Path $run 'build.log')
if ($LASTEXITCODE -ne 0) { SetupFail "build.py failed (see $run\build.log)" }
$cfg = Get-Content -Raw -LiteralPath (Join-Path $mod 'coop_join_config.h')
if ($cfg -notmatch 'COOP_PROTOCOL_VERSION (\d+)' -or [int]$Matches[1] -ne $laneProto) { SetupFail "module built for protocol $($Matches[1]), lane is $laneProto" }
$nso = Join-Path $mod 'subsdk1'
if (-not (Test-Path -LiteralPath $nso)) { SetupFail "build.py wrote no $nso" }
Res "module $nso $((Get-Item -LiteralPath $nso).Length) bytes sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $nso).Hash.Substring(0, 16)), protocol $laneProto (from $laneCommit)"

# --- Fritchii's own user folder: keys, firmware and config from the kit's emulator; kit paths rewritten; only our mod
$user = Join-Path $Emu 'user'
foreach ($d in 'keys', 'config', 'nand\system') {
    $dst = Join-Path $user $d
    if (-not (Test-Path -LiteralPath $dst)) {
        New-Item -ItemType Directory -Force $dst | Out-Null
        robocopy (Join-Path $From "user\$d") $dst /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { SetupFail "copy of $From\user\$d failed ($LASTEXITCODE)" }
    }
}
$fromUser = [IO.Path]::GetFullPath((Join-Path $From 'user')).TrimEnd('\')
$ownUser = [IO.Path]::GetFullPath($user).TrimEnd('\')
foreach ($ini in Get-ChildItem -LiteralPath (Join-Path $user 'config') -Filter '*.ini' -File) {
    $text = [IO.File]::ReadAllText($ini.FullName)
    $new = $text
    foreach ($pair in @(@($fromUser, $ownUser), @(($fromUser -replace '\\', '/'), ($ownUser -replace '\\', '/')),
            @(($fromUser -replace '\\', '\\'), ($ownUser -replace '\\', '\\')))) {
        $new = [regex]::Replace($new, [regex]::Escape($pair[0]), $pair[1].Replace('$', '$$'), 'IgnoreCase')
    }
    if ($new -ne $text) { [IO.File]::WriteAllText($ini.FullName, $new, (New-Object System.Text.UTF8Encoding $false)); Res "fritchii config: kit paths -> own user folder in $($ini.Name)" }
}
$kitRoot = [IO.Path]::GetFullPath($From).TrimEnd('\')
foreach ($form in @($kitRoot, ($kitRoot -replace '\\', '/'), ($kitRoot -replace '\\', '\\'))) {
    $left = @(Select-String -LiteralPath (Get-ChildItem -LiteralPath (Join-Path $user 'config') -Filter '*.ini' -File).FullName -SimpleMatch -Pattern $form -ErrorAction SilentlyContinue)
    if ($left.Count) { SetupFail "the Fritchii config still names the kit's folder: $($left[0].Filename):$($left[0].LineNumber)" }
}
$load = Join-Path $user "load\$title"
if (Test-Path -LiteralPath $load) { Remove-Item -Recurse -Force -LiteralPath $load }
$installed = Join-Path $load 'coop-proto\exefs\subsdk1'
New-Item -ItemType Directory -Force (Split-Path -Parent $installed) | Out-Null
Copy-Item -Force $nso $installed
if ((Get-FileHash -LiteralPath $installed).Hash -ne (Get-FileHash -LiteralPath $nso).Hash) { SetupFail "installed $installed differs from the build" }
$logDir = Join-Path $user 'log'
if (Test-Path -LiteralPath $logDir) { Get-ChildItem -LiteralPath $logDir -File | Remove-Item -Force }
Res "fritchii: $installed installed, old logs cleared"

# --- lane 1's games and server through hookshot.py (the server: the lane's own build, protocol $laneProto)
$lane = Join-Path $run 'lane'
$sc = Join-Path $repo 'tools\autotest\scenarios'
$env:OOT_COOP_TEST_PASSWORD = $password
$hargs = @('-I', "`"$(Join-Path $repo 'tools\autotest\hookshot.py')`"", 'run', '--lane', "$Lane", '--server', "`"$server`"",
           '--port', "$port", '--password', $password, '--out', "`"$lane`"", '--name', 'exefs-crossplay',
           '--script-a', "`"$sc\exefs-crossplay.A.txt`"", '--script-b', "`"$sc\exefs-crossplay.B.txt`"",
           '--load', "`"$sc\load.txt`"", '--every', '30', '--timeout', '900', '--scenario-timeout', '600',
           '--display', $td.Name, '--env', 'LICHII_COOP_PUPPET=1', '--env', 'LICHII_COOP_OBJECTS=1', '--env', 'LICHII_COOP_SPAWN=1')
$hs = Start-Process -FilePath $py -ArgumentList ($hargs -join ' ') -PassThru -WindowStyle Minimized -WorkingDirectory $repo `
    -RedirectStandardOutput (Join-Path $run 'hookshot-run.txt') -RedirectStandardError (Join-Path $run 'hookshot-run.err.txt')
Res "hookshot.py run pid $($hs.Id) -> $lane"
$srvOut = Join-Path $lane 'server-out.txt'
$fr = $null
$verdict = ''; $why = @()
try {
    # wait for the server and both Longshot players (boot + load: up to 15 minutes)
    $deadline = (Get-Date).AddMinutes(16)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $hs.HasExited) {
        Start-Sleep -Seconds 5
        if (-not (Test-Path -LiteralPath $srvOut)) { continue }
        $s = Get-Content -LiteralPath $srvOut -ErrorAction SilentlyContinue
        if (($s -match "listening on port $port") -and ($s -match 'player \d+ joined: A ') -and ($s -match 'player \d+ joined: B ')) { $ready = $true; break }
    }
    if (-not $ready) {
        Res "the lane's server never had both Longshot players (hookshot.py $(if ($hs.HasExited) { "ended, exit $($hs.ExitCode)" } else { 'still running' }))"
        $verdict = 'SETUP-FAIL'
    } else {
    Res 'server: listening, Longshot players A and B joined'
    $fr = Start-Process -FilePath (Join-Path $Emu 'fritchii.exe') -WorkingDirectory $Emu -PassThru -ArgumentList @('-g', "`"$Game`"")
    Res "fritchii pid $($fr.Id)"
    & $py -I (Join-Path $repo 'tools\autotest\placement.py') place --pid "$($fr.Id)" --wait 60 --display $td.Name 2>&1 | ForEach-Object { Res "place: $_" }

    # the module's lines (Fritchii boot + 900 frames to the join: up to 6 minutes)
    $deadline = (Get-Date).AddMinutes(6)
    while ((Get-Date) -lt $deadline -and -not $fr.HasExited -and -not $hs.HasExited) {
        Start-Sleep -Seconds 5
        $f = Get-ChildItem -LiteralPath $logDir -Filter '*.txt' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $f) { continue }
        $m = @(Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue | Where-Object { $_ -match 'coop-proto: |coop-join: ' })
        if ($m -match 'coop-join: (REFUSED|FAIL)|coop-join: disconnected') { break }
        if (($m -match 'coop-join: WELCOME: I am player') -and ($m -match "(in the session: |PLAYER_JOINED )player \d+ 'A'") -and
            ($m -match "(in the session: |PLAYER_JOINED )player \d+ 'B'")) { break }
    }
    # let pings run a minute with all three in, then a shot of the Fritchii window
    if (-not $fr.HasExited -and -not $hs.HasExited) { Start-Sleep -Seconds 60 }
    # The shot through the shared helper (coord-multiplayer\offline\lib\screen-shot.ps1: raise, own-pid 9-point cover check):
    # a covered, failed or missing shot of a running Fritchii = SETUP-FAIL (2026-11-26: raw screen copies showed the Claude app).
    if (-not $fr.HasExited) {
        $s = Save-WindowShot -Process $fr -Path (Join-Path $run 'fritchii.png') -Log (Join-Path $run 'covered.txt')
        if (-not $s -or $s.Covered -or $s.Failed) {
            Res "SETUP-FAIL shot of the Fritchii window not usable: $(if ($s) { $s.Why } else { 'no window handle or smaller than 100 px' })"
            $verdict = 'SETUP-FAIL'
        } else { Res "shot: $($s.Path)" }
    } else { Res 'shot: Fritchii already exited (the checks below say why)' }
    # the Longshot scenario runs on to its end (8 minutes): wait for hookshot.py to close the games and the server
    if (-not $hs.WaitForExit(20 * 60 * 1000)) { Res 'hookshot.py did not end in 20 minutes: stopped'; Stop-Process -Id $hs.Id -Force }
    Res "hookshot.py exit $($hs.ExitCode)"
    }
} finally {
    if ($fr -and -not $fr.HasExited) { [void]$fr.CloseMainWindow(); if (-not $fr.WaitForExit(30000)) { Res 'fritchii did not close in 30 s: stopped'; Stop-Process -Id $fr.Id -Force } }
    if (-not $hs.HasExited) { Stop-Process -Id $hs.Id -Force; Res 'hookshot.py stopped' }
    Get-Process longshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$kitLs\lane$Lane-*" } | ForEach-Object { Res "longshot pid $($_.Id) still open: stopped"; Stop-Process -Id $_.Id -Force }
    Remove-Item Env:\OOT_COOP_TEST_PASSWORD -ErrorAction SilentlyContinue
}
if ($verdict -eq 'SETUP-FAIL') { Res 'RESULT: SETUP-FAIL'; exit 2 }

# --- judge: the module's lines, the server's, each Longshot game's
$f = Get-ChildItem -LiteralPath $logDir -Filter '*.txt' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$ml = @()
if ($f) { Copy-Item -Force $f.FullName (Join-Path $run "fritchii-$($f.Name)"); $ml = @(Get-Content -LiteralPath $f.FullName | Where-Object { $_ -match 'coop-proto: |coop-join: ' }) }
foreach ($l in $ml) { Res "module: $($l.Trim())" }
$srv = @(Get-Content -LiteralPath $srvOut -ErrorAction SilentlyContinue)
foreach ($l in @($srv | Where-Object { $_ -match 'joined|left|refused|REFUSED|timed out|protocol' })) { Res "server: $($l.Trim())" }
$checks = @(
    @('module loaded by rtld', ($ml -match 'coop-proto: hello from subsdk1')),
    @('frame hook in', ($ml -match 'hook 0: frame hook in')),
    @('game value read (scene)', ($ml -match 'game value: scene ')),
    @('module got WELCOME', ($ml -match 'coop-join: WELCOME: I am player')),
    @('module sees Longshot player A', ($ml -match "(in the session: |PLAYER_JOINED )player \d+ 'A'")),
    @('module sees Longshot player B', ($ml -match "(in the session: |PLAYER_JOINED )player \d+ 'B'")),
    @('module never refused/failed', -not ($ml -match 'coop-join: (REFUSED|FAIL)')),
    @('server: emulator joined', ($srv -match 'player \d+ joined: emulator \(client: exefs-proto')))
foreach ($x in 'A', 'B') {
    $g = @(Get-ChildItem -LiteralPath $lane -Filter "hookshot_log-$x.txt" -File -ErrorAction SilentlyContinue) + @(Get-ChildItem -LiteralPath $lane -Filter "game-out-$x.txt" -File -ErrorAction SilentlyContinue)
    $seen = $false
    foreach ($gf in $g) { if (Select-String -LiteralPath $gf.FullName -Pattern "coop: player \d+ 'emulator' joined" -Quiet) { $seen = $true } }
    $checks += , @("Longshot game $x saw the emulator join", $seen)
}
$bad = @()
foreach ($c in $checks) { $ok = [bool]$c[1]; Res "$(if ($ok) { 'PASS' } else { 'FAIL' }) $($c[0])"; if (-not $ok) { $bad += $c[0] } }
$early = @($ml | Where-Object { $_ -match 'coop-join: disconnected' })
if ($early.Count) { Res "note: the module logged a disconnect: $($early[0].Trim())" }
$shots = @(Get-ChildItem -LiteralPath $lane -Filter '*.png' -Recurse -ErrorAction SilentlyContinue)
Res "shots: $($shots.Count) of the Longshot games under $lane, Fritchii: $(Test-Path -LiteralPath (Join-Path $run 'fritchii.png'))"
if ($bad.Count) { Res "RESULT: FAIL ($($bad -join ' | '))"; exit 1 }
Res 'RESULT: PASS (judge the shots too: fritchii.png in the game, the Longshot shots in the world)'
exit 0
