# v1-longshot-dropin P0 proof: co-op's first hook (the frame function, hookshot/dropin/dropin.c) attached to an
# UNMODIFIED Longshot built by the official Longshot Builder (v3.1.1 and v3.2.0, git.magicbean.dev releases), no
# Longshot rebuild for co-op. Per version: build stock with the release's own builder (--cli), check its symbols offline
# (selftest.exe), drop our winmm.dll beside longshot.exe, start the game, wait for our log's frame lines, screenshot,
# close. Unattended; exit 0 = PASS (both), 1 = FAIL, 2 = SETUP-FAIL.
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

$results = @{}
foreach ($t in $Versions) {
    $vdir = Join-Path $stockRoot $t
    $builder = Join-Path $vdir 'builder'
    $game = Join-Path $vdir 'game'
    $exe = Join-Path $game 'longshot.exe'
    # ---- 1. the stock build, by the release's own builder ----
    if (-not (Test-Path -LiteralPath $exe)) {
        if (Test-Path -LiteralPath $builder) { Remove-Item -LiteralPath $builder -Recurse -Force }
        $t0 = Get-Date
        Expand-Archive -LiteralPath (Join-Path $vdir "dl\LongshotBuilder-$t-windows-x64.zip") -DestinationPath $builder
        $bexe = Join-Path $builder 'LongshotBuilder\LongshotBuilder.exe'
        if (-not (Test-Path -LiteralPath $bexe)) { $bexe = Join-Path $builder 'LongshotBuilder.exe' }
        $blog = Join-Path $out "build-$t.log"
        # (ONE quoted argument string: Start-Process -ArgumentList with an array does not quote paths with spaces)
        $args1 = "--cli --nup `"$nup`" --out `"$game`" --log `"$blog`" --jobs $Jobs"
        Say "$t build: $bexe $args1"
        $bp = Start-Process -FilePath $bexe -ArgumentList $args1 -WorkingDirectory (Split-Path -Parent $bexe) -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $out "build-$t.stdout.txt") -RedirectStandardError (Join-Path $out "build-$t.stderr.txt")
        if (-not $bp.WaitForExit(90 * 60 * 1000)) { $bp.Kill(); Say "$t build: over 90 min, stopped"; $results[$t] = 'FAIL build timeout'; continue }
        $mins = [Math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
        Say "$t build: exit $($bp.ExitCode) after $mins min (unzip to built game)"
        if (-not (Test-Path -LiteralPath $exe)) { $results[$t] = "FAIL stock build made no longshot.exe (exit $($bp.ExitCode))"; continue }
    } else { Say "$t build: kept from an earlier run ($exe)" }
    # ---- 2. offline: the stock exe's own symbols and prologues ----
    $st = & $self $exe 2>&1; $st | ForEach-Object { Say "$t selftest: $_" }
    if ($LASTEXITCODE -ne 0) { $results[$t] = 'FAIL stock exe: symbols or prologues (selftest)'; continue }
    # ---- 3. drop in, start, wait for our lines ----
    Copy-Item -LiteralPath $dll -Destination (Join-Path $game 'winmm.dll') -Force
    $dlog = Join-Path $game 'squadala-dropin.log'
    if (Test-Path -LiteralPath $dlog) { Remove-Item -LiteralPath $dlog -Force }
    $t1 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru
    Say "$t game: pid $($p.Id)"
    $deadline = (Get-Date).AddSeconds(90); $attached = $false
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        if ((Test-Path -LiteralPath $dlog) -and (Select-String -LiteralPath $dlog -Pattern 'dropin: attached' -Quiet)) { $attached = $true; break }
        Start-Sleep -Seconds 2
    }
    if (-not $attached) {
        Say "$t game: no 'dropin: attached' line in 90 s (exited: $($p.HasExited)): our winmm.dll was not loaded"
        if (-not $p.HasExited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(20000)) { $p.Kill() } }
        $results[$t] = 'FAIL not loaded'; continue
    }
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
    $glog = Join-Path $game 'logs\longshot_log.txt'
    $exited = $p.HasExited
    if (-not $exited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(30000)) { Say "$t game: did not close in 30 s: killed"; $p.Kill() } }
    if (Test-Path -LiteralPath $glog) {
        Copy-Item -LiteralPath $glog -Destination (Join-Path $out "$t-longshot_log.txt") -Force
        Select-String -LiteralPath $glog -Pattern 'fatal|exception 0x|no function at' | Select-Object -First 5 | ForEach-Object { Say "$t game log: $($_.Line)" }
    }
    $wordOk = Select-String -LiteralPath $dlog -Pattern "the game's pacibsp: hook on" -Quiet
    if ($exited) { $results[$t] = "FAIL the game exited by itself (see $t-longshot_log.txt)" }
    elseif (-not $framed) { $results[$t] = 'FAIL no frame 600 line in 240 s' }
    elseif (-not $wordOk) { $results[$t] = 'FAIL guest word check' }
    elseif (-not $shotOk) { $results[$t] = 'FAIL no screenshot' }
    else { $results[$t] = "PASS frame 600 at $secs s after start, guest word read, shot $t-game.png" }
    Remove-Item -LiteralPath (Join-Path $game 'winmm.dll') -Force   # the stock folder as the builder left it
    Say "$t RESULT $($results[$t])"
}
$all = @($Versions | Where-Object { $results[$_] -notlike 'PASS*' }).Count -eq 0
Say ("RESULT " + $(if ($all) { 'PASS' } else { 'FAIL' }) + ' | ' + (($Versions | ForEach-Object { "$_ $($results[$_])" }) -join ' | '))
Say "stock games kept in $stockRoot (a rerun reuses them; delete by hand when done)"
exit $(if ($all) { 0 } else { 1 })

