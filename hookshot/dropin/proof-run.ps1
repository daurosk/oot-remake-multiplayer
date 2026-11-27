# SPDX-License-Identifier: GPL-3.0-or-later
# v1-longshot-dropin P0 proof: co-op's first hook (the frame function, hookshot/dropin/dropin.c) attached to an
# UNMODIFIED Longshot built by the official Longshot Builder (v3.1.1 and v3.2.0, git.magicbean.dev releases), no
# Longshot rebuild for co-op. Per version: build stock with the release's own builder (--cli), check its symbols offline
# (selftest.exe), drop our winmm.dll beside the game's bin\longshot.exe, start the game, wait for our log's frame lines,
# screenshot, close; winmm.dll removed again on every path. Unattended; exit 0 = PASS (both), 1 = FAIL, 2 = SETUP-FAIL.
# GAME STARTS moved to the bridge pytest test tools/autotest/ingame/test_dropin_longshot.py (worker rule 2026-11-26:
# every in-game run is a pytest test): this script now runs ONLY with -SetupOnly or -BuildOnly (the official tools and the
# stock build); without either it stops at once as SETUP-FAIL.
# Usage: powershell -ExecutionPolicy Bypass -File hookshot\dropin\proof-run.ps1 -Expect <sha of this checkout> [-Versions v3.1.1,v3.2.0] [-Display \\.\DISPLAY2] [-SetupOnly] [-BuildOnly] [-ShortRoot C:\lsb]
# -SetupOnly: the setup proof and the official setup's --check only (installs the tools, checks the game file): no build, no game.
# -BuildOnly: also the stock build and the offline symbol check (selftest) of its exe; no game start, nothing dropped in.
# -Expect <sha>: this checkout's HEAD must start with it and have no uncommitted tracked changes, else SETUP-FAIL (a queued run
#  runs exactly the reviewed commit; same check as tools\switch_mod\run-crossplay.ps1).
param([Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$Expect,
    [string[]]$Versions = @('v3.1.1', 'v3.2.0'), [string]$Display = '', [int]$Jobs = 0, [switch]$SetupOnly, [switch]$BuildOnly, [string]$ShortRoot = '', [switch]$FullSpeed)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))        # ...\OOT REMAKE
$coord = Join-Path $root 'coord-multiplayer'
$stockRoot = Join-Path $root 'dropin-stock'                                         # the official zips: dropin-stock\<tag>\dl, unzipped to \builder
# Everything the official setup and build WRITE goes under a short root (<short>\<tag>\home, \game, \work): under
# dropin-stock the setup's portable home made a 262-character path (measured 2026-11-26 on the v3.2.0 install: ...\builder\
# LongshotBuilder-data\versions\v3.2.0\LongshotBuilder\tools\third_party\fidelityfx-sdk\...\ffx_frameinterpolation_
# reconstruct_dilated_velocity_and_previous_depth.h) and this PC has LongPathsEnabled=0. The setup takes its home from
# LONGSHOT_SETUP_BASE when --portable is given (Longshot v3.2.0 and v3.1.1 builder/setup/setup.c init_home: "if (portable) {
# if (*requested) { if (!hb_home_full(g_home.root, requested)) return 0; }"), and stops with "A path in this portable
# installation is too long for the Windows setup" at MAX_PATH (setup.c home_path).
if (-not $ShortRoot) { $ShortRoot = $(if ($env:OOT_DROPIN_SHORT_ROOT) { $env:OOT_DROPIN_SHORT_ROOT } else { 'C:\lsb' }) }
# The player's game: the .nup in the OOT REMAKE folder (dir listing 2026-11-25; the kits' own builds take the same file)
$nup = Join-Path $root 'The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup'
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$out = Join-Path $coord "offline\v1-dropin-proof\$stamp"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$logFile = Join-Path $out 'run.log'
function Say([string]$m) { $l = (Get-Date).ToUniversalTime().ToString('HH:mm:ss') + ' ' + $m; Write-Host $l; Add-Content -LiteralPath $logFile -Value $l -Encoding utf8 }
function SetupFail([string]$m) { Say "SETUP-FAIL $m"; Say "RESULT SETUP-FAIL $m"; exit 2 }

# ---- setup proof (each check < 2 min) -------------------------------------------------------------------------------
if (-not $SetupOnly -and -not $BuildOnly) { SetupFail 'game starts are the pytest test tools/autotest/ingame/test_dropin_longshot.py (--env BT_DROPIN=1); use -SetupOnly or -BuildOnly here' }
Say "v1-dropin-proof $stamp versions $($Versions -join ',') out $out"
$repo = Split-Path -Parent (Split-Path -Parent $here)                                # this checkout (hookshot\dropin -> repo)
$sha = try { "$(git -C $repo rev-parse HEAD)".Trim() } catch { '' }
if (-not $sha -or -not $sha.ToLower().StartsWith($Expect.ToLower())) { SetupFail "checkout $repo is at '$sha', not $Expect" }
$dirty = @(git -C $repo status --porcelain --untracked-files=no)
if ($dirty.Count) { SetupFail "checkout $repo has uncommitted changes ($($dirty[0]))" }
Say "checkout $repo at $sha (expected $Expect)"
if (-not (Test-Path -LiteralPath $nup)) { SetupFail "no game file at $nup" }
$dll = Join-Path $here 'out\winmm.dll'
$self = Join-Path $here 'out\selftest.exe'
foreach ($f in @($dll, $self)) { if (-not (Test-Path -LiteralPath $f)) { SetupFail "missing $f (run hookshot\dropin\build.ps1 and compile selftest.c)" } }
Say ("our winmm.dll sha256 " + (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)
try { New-Item -ItemType Directory -Force -Path $ShortRoot | Out-Null; $ShortRoot = (Resolve-Path -LiteralPath $ShortRoot).Path.TrimEnd('\') }
catch { SetupFail "cannot make the short root $ShortRoot ($($_.Exception.Message)); give another with -ShortRoot" }
# (0 or missing: Windows refuses paths of 260 and more for most programs; the documented LongPathsEnabled value)
$longPaths = [int]((Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled -ErrorAction SilentlyContinue).LongPathsEnabled)
Say "short root $ShortRoot, LongPathsEnabled=$longPaths"
$free = (Get-PSDrive -Name ($ShortRoot.Substring(0, 1))).Free
if ($free -lt 130GB) { SetupFail ("free disk {0:N0} GB < 130 GB (two stock builds, ~60 GB each while building)" -f ($free / 1GB)) }
$turn = Get-Content -LiteralPath (Join-Path $coord 'TEST-TURN') -TotalCount 5 | Where-Object { $_ -match 'LANE1:' } | Select-Object -First 1
Say "TEST-TURN: $turn"
# (a game start only when the lanes are open: the coordinator writes "NO game starts" / "OWNER PLAYING" in that line while the owner plays)
if (-not $SetupOnly -and -not $BuildOnly -and "$turn" -match 'NO game starts|OWNER PLAYING') {
    SetupFail 'TEST-TURN says no game starts now: use -BuildOnly (or -SetupOnly), or wait for the lanes to open'
}
. (Join-Path $coord 'offline\lib\test-display.ps1')
try { $disp = Get-TestDisplay -Display $Display } catch { SetupFail $_.Exception.Message }
if (-not $disp.Bounds) { SetupFail "display $($disp.Name) has no bounds" }
Say "display $($disp.Name) $($disp.Width)x$($disp.Height) ($($disp.Reason))"
# Window shots through the shared helper (raise, own-pid 9-point cover check; 2026-11-26: raw screen copies showed the Claude app)
$shotLib = Join-Path $coord 'offline\lib\screen-shot.ps1'
if (-not (Test-Path -LiteralPath $shotLib)) { SetupFail "missing $shotLib (the shared window-shot helper)" }
. $shotLib
$script:shotWhy = ''
if (-not $SetupOnly -and -not $BuildOnly) { foreach ($l in (Clear-TestScreen -Bounds $disp.Bounds)) { Say "test screen $($disp.Name): $l" } }   # (minimised, none closed)
# (the same order: ONE local compile on the PC at a time, held by the folder oot-remake-multiplayer\build.lock, 4 jobs at most)
# -FullSpeed (only while the owner is away; TEST-TURN says when the 4-job/Idle limit is lifted): -Jobs as given, else all
# CPU threads but four, at Normal priority. build.lock is taken either way.
if ($FullSpeed) { if ($Jobs -le 0) { $Jobs = [Math]::Max(2, [Environment]::ProcessorCount - 4) } }
elseif ($Jobs -le 0 -or $Jobs -gt 4) { $Jobs = 4 }
Say "build: $Jobs jobs, $(if ($FullSpeed) { 'Normal' } else { 'Idle' }) priority"
$buildLock = Join-Path $root 'oot-remake-multiplayer\build.lock'
foreach ($t in $Versions) {
    $dl = Join-Path $stockRoot "$t\dl"
    $zip = Join-Path $dl "LongshotBuilder-$t-windows-x64.zip"
    $sums = Join-Path $dl 'SHA256SUMS'
    if (-not (Test-Path -LiteralPath $zip) -or -not (Test-Path -LiteralPath $sums)) { SetupFail "no official builder zip/SHA256SUMS for $t in $dl" }
    $want = (Get-Content -LiteralPath $sums | Where-Object { $_ -match [regex]::Escape("LongshotBuilder-$t-windows-x64.zip") } | Select-Object -First 1) -split '\s+' | Select-Object -First 1
    $have = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    if (-not $want -or $have -ne $want.ToUpper()) { SetupFail "$t builder zip sha256 $have is not the release's $want" }
    Say "$t builder zip sha256 OK ($have)"
    # path length: the setup copies the zip's LongshotBuilder\ into <home>\versions\<tag>.part\ and renames it to <tag> when
    # complete (setup.c: home_path(install, L"%ls\\%ls.part", versions, wtag)); the longest result must stay under 260
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try { $longest = $z.Entries | Sort-Object { $_.FullName.Length } -Descending | Select-Object -First 1 -ExpandProperty FullName } finally { $z.Dispose() }
    $deep = (Join-Path $ShortRoot "$t\home\versions\$t.part\").Length + $longest.Length
    $unzip = (Join-Path $stockRoot "$t\builder\").Length + $longest.Length
    Say "$t longest path: $deep in the setup's home, $unzip unzipped (zip entry $($longest.Length): $longest)"
    if ($longPaths -ne 1 -and ($deep -ge 260 -or $unzip -ge 260)) {
        SetupFail "$t a path would be $([Math]::Max($deep, $unzip)) characters (limit 259, LongPathsEnabled=$longPaths): give a shorter -ShortRoot than $ShortRoot"
    }
}
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class DW { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; } }
'@
function Shot([System.Diagnostics.Process]$p, [string]$path) {
    # a covered, failed or missing shot sets $script:shotWhy -> that version's result is SETUP-FAIL (exit 2), not FAIL
    $script:shotWhy = ''
    $s = Save-WindowShot -Process $p -Path $path -Log (Join-Path (Split-Path -Parent $path) 'covered.txt')
    if (-not $s) { $script:shotWhy = 'no window handle or smaller than 100 px'; Say "shot: $($script:shotWhy)"; return $false }
    if ($s.Covered -or $s.Failed) { $script:shotWhy = $s.Why; Say "shot: NOT USABLE $($s.Path): $($s.Why)"; return $false }
    Say "shot: $($s.Path)"; return $true
}


# The official builder, as its own source says (Longshot v3.2.0 builder/setup/setup.c, the zip's top LongshotBuilder.exe):
# "only --cli installs and starts it" (setup.c:1-14: copies the builder, downloads each tools.lock tool, checks its SHA-256,
# then runs LongshotBuilder\LongshotBuilder.exe); "--portable is an initial setup option, before the app/CLI arguments"
# (setup.c:278), its home in the setup's own folder, so nothing goes to %LOCALAPPDATA%. The inner
# LongshotBuilder\LongshotBuilder.exe alone has no tools ("Python was not found": review-b121 probe; Program.cs Cli:
# "The build tools were not found next to this program"). Its arguments after --cli are make_game.py's
# (Program.cs:19 "--cli <make_game.py's options>"): --nup, --out, --work (default ~/.cache), --jobs, --language, --log
# (nx2/make_game.py:2824-2857), --check "only checks your game's version" (docs/user/advanced.md:86).
# The built game: <out>\longshot.exe is the setup's copy (a launcher); the game is <out>\bin\longshot.exe, started with the
# game folder as its working folder (setup.c:865 play_folder / the "game folder's launcher" block in wWinMain).
function Run-Setup([string]$setup, [string]$homeDir, [string]$cliArgs, [string]$tag, [int]$minutes) {
    # (ONE quoted argument string: Start-Process -ArgumentList with an array does not quote paths with spaces)
    $a = "--portable --cli $cliArgs"
    $env:LONGSHOT_SETUP_BASE = $homeDir          # (the setup's home with --portable: setup.c init_home; Start-Process inherits it)
    Say "$tag setup: LONGSHOT_SETUP_BASE=$homeDir $setup $a"
    # Idle priority for the setup and every compiler under it (owner 2026-11-26, COORD-LOG "C #122 03:16": local compiles
    # dropped his game's fps): a child of an Idle-class process starts in the Idle class (CreateProcess), so this script
    # lowers itself for the start and goes back to Normal after it.
    $me = Get-Process -Id $PID; if (-not $FullSpeed) { $me.PriorityClass = 'Idle' }
    try {
        $p = Start-Process -FilePath $setup -ArgumentList $a -WorkingDirectory (Split-Path -Parent $setup) -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $out "$tag.stdout.txt") -RedirectStandardError (Join-Path $out "$tag.stderr.txt")
    } finally { $me.PriorityClass = 'Normal' }
    Say "$tag setup: pid $($p.Id) priority $($p.PriorityClass)"
    # (Windows PowerShell 5.1: without the handle taken while the process lives, ExitCode stays empty after -PassThru;
    #  seen in run 20261126-031841: "--check exit " with nothing, although the setup printed "@@check ok")
    $null = $p.Handle
    if (-not $p.WaitForExit($minutes * 60 * 1000)) { $p.Kill(); Say "$tag setup: over $minutes min, stopped"; return -1 }
    Get-Content -LiteralPath (Join-Path $out "$tag.stdout.txt") -Tail 5 -ErrorAction SilentlyContinue | ForEach-Object { Say "$tag stdout: $_" }
    Get-Content -LiteralPath (Join-Path $out "$tag.stderr.txt") -Tail 3 -ErrorAction SilentlyContinue | ForEach-Object { Say "$tag stderr: $_" }
    $p.WaitForExit()                              # (the redirected output fully written, the exit code set)
    if ($null -eq $p.ExitCode) { Say "$tag setup: no exit code from the process object"; return -2 }
    return [int]$p.ExitCode
}

$results = @{}
foreach ($t in $Versions) {
    $builder = Join-Path $stockRoot "$t\builder"          # the unzipped official zip (only read from here on)
    $vdir = Join-Path $ShortRoot $t                        # all that the setup and the build write
    $homeDir = Join-Path $vdir 'home'
    $game = Join-Path $vdir 'game'
    $work = Join-Path $vdir 'work'
    $bin = Join-Path $game 'bin'
    $exe = Join-Path $bin 'longshot.exe'
    $placed = Join-Path $bin 'winmm.dll'
    $p = $null
    try {
        # ---- 1. the stock build, by the release's own setup + builder ----
        if ($SetupOnly -or -not (Test-Path -LiteralPath $exe)) {
            if (-not (Test-Path -LiteralPath (Join-Path $builder 'tools.lock'))) {
                if (Test-Path -LiteralPath $builder) { Remove-Item -LiteralPath $builder -Recurse -Force }
                Expand-Archive -LiteralPath (Join-Path $stockRoot "$t\dl\LongshotBuilder-$t-windows-x64.zip") -DestinationPath $builder
            }
            $setup = Join-Path $builder 'LongshotBuilder.exe'
            if (-not (Test-Path -LiteralPath $setup) -or -not (Test-Path -LiteralPath (Join-Path $builder 'tools.lock'))) {
                SetupFail "$t zip has no top LongshotBuilder.exe + tools.lock in $builder"
            }
            $t0 = Get-Date
            # setup step: installs the tools tools.lock names and checks the game file; any failure here is SETUP-FAIL
            $c = Run-Setup $setup $homeDir "--check --nup `"$nup`" --log `"$(Join-Path $out "check-$t.log")`"" "check-$t" 30
            if ($c -ne 0) { SetupFail "$t official setup --check exit $c (tools not installed or game not accepted; see check-$t.*)" }
            Say "$t setup + --check OK after $([Math]::Round(((Get-Date) - $t0).TotalMinutes, 1)) min"
            if ($SetupOnly) { $results[$t] = 'PASS setup only (official tools installed, game file checked; no build, no game start)'; continue }
            # the compile: only with build.lock (a folder: New-Item fails when another session holds it); waits up to 2 h
            $lockEnd = (Get-Date).AddMinutes(120); $locked = $false
            while (-not $locked) {
                try { New-Item -ItemType Directory -Path $buildLock -ErrorAction Stop | Out-Null; $locked = $true }
                catch { if ((Get-Date) -gt $lockEnd) { break }; Start-Sleep -Seconds 30 }
            }
            if (-not $locked) { $results[$t] = "FAIL build.lock held by another session for 2 h ($buildLock\owner.txt)"; continue }
            try {
                Set-Content -LiteralPath (Join-Path $buildLock 'owner.txt') -Encoding utf8 -Value "v1-dropin-proof $stamp stock $t build, pid $PID, $((Get-Date).ToUniversalTime().ToString('HH:mm')) UTC, $Jobs jobs, $(if ($FullSpeed) { 'Normal' } else { 'Idle' })"
                Say "$t build.lock taken"
                $c = Run-Setup $setup $homeDir ("--nup `"$nup`" --out `"$game`" --work `"$work`" --language en-US --jobs $Jobs " +
                                       "--log `"$(Join-Path $out "build-$t.log")`"") "build-$t" 240
            } finally { Remove-Item -LiteralPath $buildLock -Recurse -Force; Say "$t build.lock given back" }
            $mins = [Math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
            Say "$t build: exit $c after $mins min (unzip to built game)"
            if (-not (Test-Path -LiteralPath $exe)) { $results[$t] = "FAIL stock build made no bin\longshot.exe (exit $c)"; continue }
        } else { Say "$t build: kept from an earlier run ($exe)" }
        # ---- 2. offline: the stock exe's own symbols and prologues ----
        $st = & $self $exe 2>&1; $st | ForEach-Object { Say "$t selftest: $_" }
        if ($LASTEXITCODE -ne 0) { $results[$t] = 'FAIL stock exe: symbols or prologues (selftest)'; continue }
        if ($BuildOnly) { $results[$t] = 'PASS build only (stock game built, selftest OK; no game start)'; continue }
        # ---- 3. drop in, start, wait for our lines ----
        Copy-Item -LiteralPath $dll -Destination $placed -Force
        $dlog = Join-Path $bin 'squadala-dropin.log'
        if (Test-Path -LiteralPath $dlog) { Remove-Item -LiteralPath $dlog -Force }
        $t1 = Get-Date
        $p = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru
        Say "$t game: pid $($p.Id)"
        $deadline = (Get-Date).AddSeconds(90); $attached = $false
        while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
            if ((Test-Path -LiteralPath $dlog) -and (Select-String -LiteralPath $dlog -Pattern 'dropin: attached' -Quiet)) { $attached = $true; break }
            Start-Sleep -Seconds 2
        }
        if (-not $attached) { Say "$t game: no 'dropin: attached' line in 90 s (exited: $($p.HasExited)): our winmm.dll was not loaded"; $results[$t] = 'FAIL not loaded'; continue }
        $p.Refresh()
        $wait = (Get-Date).AddSeconds(30)
        while ($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date) -lt $wait -and -not $p.HasExited) { Start-Sleep -Milliseconds 500; $p.Refresh() }
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            $b = $disp.Bounds; [void][DW]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $b.X + 40, $b.Y + 40, 1280, 720, 0x0044)
        }
        $deadline = (Get-Date).AddSeconds(240); $framed = $false
        while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
            if (Select-String -LiteralPath $dlog -Pattern 'dropin: frame 600 ' -Quiet) { $framed = $true; break }
            Start-Sleep -Seconds 2
        }
        $secs = [Math]::Round(((Get-Date) - $t1).TotalSeconds)
        $shotOk = $false
        if (-not $p.HasExited) { Start-Sleep -Seconds 5; $shotOk = Shot $p (Join-Path $out "$t-game.png") }
        Copy-Item -LiteralPath $dlog -Destination (Join-Path $out "$t-squadala-dropin.log") -Force
        Get-Content -LiteralPath $dlog | ForEach-Object { Say "$t dropin.log: $_" }
        $exited = $p.HasExited
        $wordOk = Select-String -LiteralPath $dlog -Pattern "the game's pacibsp: hook on" -Quiet
        if ($exited) { $results[$t] = "FAIL the game exited by itself (see $t-longshot_log.txt)" }
        elseif (-not $framed) { $results[$t] = 'FAIL no frame 600 line in 240 s' }
        elseif (-not $wordOk) { $results[$t] = 'FAIL guest word check' }
        elseif (-not $shotOk) { $results[$t] = "SETUP-FAIL no usable screenshot ($($script:shotWhy))" }
        else { $results[$t] = "PASS frame 600 at $secs s after start, guest word read, shot $t-game.png" }
    } finally {
        # every path: the game closed, its log kept, our winmm.dll out of the stock folder
        if ($p -and -not $p.HasExited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(30000)) { Say "$t game: did not close in 30 s: killed"; $p.Kill() } }
        # (the game log: logs\longshot_log.txt of the game folder, Longshot v3.2.0 host/hos_main.c:2178 + host/game_layout.c:6;
        #  older layouts wrote it beside the exe (collect-logs.ps1:89): all four places checked)
        $glogs = @((Join-Path $game 'logs\longshot_log.txt'), (Join-Path $bin 'logs\longshot_log.txt'), (Join-Path $bin 'longshot_log.txt'), (Join-Path $game 'longshot_log.txt')) |
            Where-Object { Test-Path -LiteralPath $_ }
        if (-not $glogs) { Say "$t game log: none found (game\logs, bin\logs, bin, game)" }
        foreach ($glog in $glogs) {
            Say "$t game log: $glog"
            Copy-Item -LiteralPath $glog -Destination (Join-Path $out ("$t-" + ($glog.Substring($vdir.Length + 1) -replace '\\', '_'))) -Force
            Select-String -LiteralPath $glog -Pattern 'fatal|exception 0x|no function at' | Select-Object -First 5 | ForEach-Object { Say "$t game log: $($_.Line)" }
        }
        if (Test-Path -LiteralPath $placed) { Remove-Item -LiteralPath $placed -Force; Say "$t winmm.dll removed from $bin" }
        if ($results.ContainsKey($t)) { Say "$t RESULT $($results[$t])" }
    }
}
$all = @($Versions | Where-Object { "$($results[$_])" -notlike 'PASS*' }).Count -eq 0
$setupBad = @($Versions | Where-Object { "$($results[$_])" -like 'SETUP-FAIL*' }).Count -gt 0
$word = if ($all) { 'PASS' } elseif ($setupBad) { 'SETUP-FAIL' } else { 'FAIL' }
Say ("RESULT $word | " + (($Versions | ForEach-Object { "$_ $($results[$_])" }) -join ' | '))
Say "stock games kept in $ShortRoot, the official zips in $stockRoot (a rerun reuses them; delete by hand when done)"
exit $(if ($all) { 0 } elseif ($setupBad) { 2 } else { 1 })
