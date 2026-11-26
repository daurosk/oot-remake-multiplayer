# SPDX-License-Identifier: GPL-3.0-or-later
# v1-longshot-dropin P0 proof: co-op's first hook (the frame function, hookshot/dropin/dropin.c) attached to an
# UNMODIFIED Longshot built by the official Longshot Builder (v3.1.1 and v3.2.0, git.magicbean.dev releases), no
# Longshot rebuild for co-op. Per version: build stock with the release's own builder (--cli), check its symbols offline
# (selftest.exe), drop our winmm.dll beside the game's bin\longshot.exe, start the game, wait for our log's frame lines,
# screenshot, close; winmm.dll removed again on every path. Unattended; exit 0 = PASS (both), 1 = FAIL, 2 = SETUP-FAIL.
# Usage: powershell -ExecutionPolicy Bypass -File hookshot\dropin\proof-run.ps1 [-Versions v3.1.1,v3.2.0] [-Display \\.\DISPLAY2]
param([string[]]$Versions = @('v3.1.1', 'v3.2.0'), [string]$Display = '', [int]$Jobs = 0)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))        # ...\OOT REMAKE
$coord = Join-Path $root 'coord-multiplayer'
$stockRoot = Join-Path $root 'dropin-stock'                                         # the official zips: dropin-stock\<tag>\dl
# The player's game: the .nup in the OOT REMAKE folder (dir listing 2026-11-25; the kits' own builds take the same file)
$nup = Join-Path $root 'The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup'
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$out = Join-Path $coord "offline\v1-dropin-proof\$stamp"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$logFile = Join-Path $out 'run.log'
function Say([string]$m) { $l = (Get-Date).ToUniversalTime().ToString('HH:mm:ss') + ' ' + $m; Write-Output $l; Add-Content -LiteralPath $logFile -Value $l -Encoding utf8 }
function SetupFail([string]$m) { Say "SETUP-FAIL $m"; Say "RESULT SETUP-FAIL $m"; exit 2 }

# ---- setup proof (each check < 2 min) -------------------------------------------------------------------------------
Say "v1-dropin-proof $stamp versions $($Versions -join ',') out $out"
if (-not (Test-Path -LiteralPath $nup)) { SetupFail "no game file at $nup" }
$dll = Join-Path $here 'out\winmm.dll'
$self = Join-Path $here 'out\selftest.exe'
foreach ($f in @($dll, $self)) { if (-not (Test-Path -LiteralPath $f)) { SetupFail "missing $f (run hookshot\dropin\build.ps1 and compile selftest.c)" } }
Say ("our winmm.dll sha256 " + (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)
$free = (Get-PSDrive -Name ($root.Substring(0, 1))).Free
if ($free -lt 130GB) { SetupFail ("free disk {0:N0} GB < 130 GB (two stock builds, ~60 GB each while building)" -f ($free / 1GB)) }
$turn = Get-Content -LiteralPath (Join-Path $coord 'TEST-TURN') -TotalCount 5 | Where-Object { $_ -match 'LANE1:' } | Select-Object -First 1
Say "TEST-TURN: $turn"
. (Join-Path $coord 'offline\lib\test-display.ps1')
try { $disp = Get-TestDisplay -Display $Display } catch { SetupFail $_.Exception.Message }
if (-not $disp.Bounds) { SetupFail "display $($disp.Name) has no bounds" }
Say "display $($disp.Name) $($disp.Width)x$($disp.Height) ($($disp.Reason))"
if ($Jobs -le 0) { $Jobs = [Math]::Max(2, [Environment]::ProcessorCount - 4) }
foreach ($t in $Versions) {
    $dl = Join-Path $stockRoot "$t\dl"
    $zip = Join-Path $dl "LongshotBuilder-$t-windows-x64.zip"
    $sums = Join-Path $dl 'SHA256SUMS'
    if (-not (Test-Path -LiteralPath $zip) -or -not (Test-Path -LiteralPath $sums)) { SetupFail "no official builder zip/SHA256SUMS for $t in $dl" }
    $want = (Get-Content -LiteralPath $sums | Where-Object { $_ -match [regex]::Escape("LongshotBuilder-$t-windows-x64.zip") } | Select-Object -First 1) -split '\s+' | Select-Object -First 1
    $have = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    if (-not $want -or $have -ne $want.ToUpper()) { SetupFail "$t builder zip sha256 $have is not the release's $want" }
    Say "$t builder zip sha256 OK ($have)"
}
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class DW { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; } }
'@
function Shot([System.Diagnostics.Process]$p, [string]$path) {
    $p.Refresh(); $r = New-Object DW+RECT
    if (-not [DW]::GetWindowRect($p.MainWindowHandle, [ref]$r)) { Say "shot: no window rect"; return $false }
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { Say "shot: empty window"; return $false }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); Say "shot: $path ${w}x$h"; return $true
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
function Run-Setup([string]$setup, [string]$cliArgs, [string]$tag, [int]$minutes) {
    # (ONE quoted argument string: Start-Process -ArgumentList with an array does not quote paths with spaces)
    $a = "--portable --cli $cliArgs"
    Say "$tag setup: $setup $a"
    $p = Start-Process -FilePath $setup -ArgumentList $a -WorkingDirectory (Split-Path -Parent $setup) -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $out "$tag.stdout.txt") -RedirectStandardError (Join-Path $out "$tag.stderr.txt")
    if (-not $p.WaitForExit($minutes * 60 * 1000)) { $p.Kill(); Say "$tag setup: over $minutes min, stopped"; return -1 }
    Get-Content -LiteralPath (Join-Path $out "$tag.stdout.txt") -Tail 5 -ErrorAction SilentlyContinue | ForEach-Object { Say "$tag stdout: $_" }
    Get-Content -LiteralPath (Join-Path $out "$tag.stderr.txt") -Tail 3 -ErrorAction SilentlyContinue | ForEach-Object { Say "$tag stderr: $_" }
    return $p.ExitCode
}

$results = @{}
foreach ($t in $Versions) {
    $vdir = Join-Path $stockRoot $t
    $builder = Join-Path $vdir 'builder'
    $game = Join-Path $vdir 'game'
    $work = Join-Path $vdir 'work'
    $bin = Join-Path $game 'bin'
    $exe = Join-Path $bin 'longshot.exe'
    $placed = Join-Path $bin 'winmm.dll'
    $p = $null
    try {
        # ---- 1. the stock build, by the release's own setup + builder ----
        if (-not (Test-Path -LiteralPath $exe)) {
            if (-not (Test-Path -LiteralPath (Join-Path $builder 'tools.lock'))) {
                if (Test-Path -LiteralPath $builder) { Remove-Item -LiteralPath $builder -Recurse -Force }
                Expand-Archive -LiteralPath (Join-Path $vdir "dl\LongshotBuilder-$t-windows-x64.zip") -DestinationPath $builder
            }
            $setup = Join-Path $builder 'LongshotBuilder.exe'
            if (-not (Test-Path -LiteralPath $setup) -or -not (Test-Path -LiteralPath (Join-Path $builder 'tools.lock'))) {
                SetupFail "$t zip has no top LongshotBuilder.exe + tools.lock in $builder"
            }
            $t0 = Get-Date
            # setup step: installs the tools tools.lock names and checks the game file; any failure here is SETUP-FAIL
            $c = Run-Setup $setup "--check --nup `"$nup`" --log `"$(Join-Path $out "check-$t.log")`"" "check-$t" 30
            if ($c -ne 0) { SetupFail "$t official setup --check exit $c (tools not installed or game not accepted; see check-$t.*)" }
            Say "$t setup + --check OK after $([Math]::Round(((Get-Date) - $t0).TotalMinutes, 1)) min"
            $c = Run-Setup $setup ("--nup `"$nup`" --out `"$game`" --work `"$work`" --language en-US --jobs $Jobs " +
                                   "--log `"$(Join-Path $out "build-$t.log")`"") "build-$t" 90
            $mins = [Math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
            Say "$t build: exit $c after $mins min (unzip to built game)"
            if (-not (Test-Path -LiteralPath $exe)) { $results[$t] = "FAIL stock build made no bin\longshot.exe (exit $c)"; continue }
        } else { Say "$t build: kept from an earlier run ($exe)" }
        # ---- 2. offline: the stock exe's own symbols and prologues ----
        $st = & $self $exe 2>&1; $st | ForEach-Object { Say "$t selftest: $_" }
        if ($LASTEXITCODE -ne 0) { $results[$t] = 'FAIL stock exe: symbols or prologues (selftest)'; continue }
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
        elseif (-not $shotOk) { $results[$t] = 'FAIL no screenshot' }
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
Say ("RESULT " + $(if ($all) { 'PASS' } else { 'FAIL' }) + ' | ' + (($Versions | ForEach-Object { "$_ $($results[$_])" }) -join ' | '))
Say "stock games kept in $stockRoot (a rerun reuses them; delete by hand when done)"
exit $(if ($all) { 0 } else { 1 })
