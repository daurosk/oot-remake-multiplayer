<#
.SYNOPSIS
Runs a two-game co-op test with no one at the keyboard and keeps screenshots of both games.

.DESCRIPTION
Starts the co-op server and two kits (A and B) on this PC, waits until both have joined the
server, plays scenarios\load.txt in both (title screen to the first save), then the scenario's
input scripts (scenarios\<name>.A.txt and, if present, <name>.B.txt). The emulator plays each
script into its own pad (LICHII_COOP_TEST_SCRIPT; see lichii/hook/coop_testinput.h), so the
windows need no focus. Both windows are captured every -Every seconds and at each `mark` line,
into <shots>\<scenario>\<time>\, with the emulator and server logs. Both games and the server are
closed at the end, then the folder is printed. See docs/testing.md, "Automatic tests".

.EXAMPLE
tools\autotest\run.ps1 -Scenario walk
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    # The kits: folders with emulator\lichii.exe and a filled emulator\user\ (keys, firmware, save).
    # Default: auto-A and auto-B in the first coop-kit folder found beside the repo or above it.
    [string]$KitA = "",
    [string]$KitB = "",
    # The game file (.nup, .nsp or .xci). Default: the first .nup in the folder above coop-kit.
    [string]$Game = "",
    # An emulator build to copy into both kits as emulator\lichii.exe first (optional).
    [string]$Exe = "",
    [string]$Server = "$PSScriptRoot\..\..\build\server\coop_server.exe",
    [int]$Port = 27560,
    [string]$Shots = "",
    # Seconds between screenshots while the scenario plays.
    [double]$Every = 3,
    # LICHII_COOP_... switches for both games, as NAME=value (replaces the default list).
    # As NAME=value: an array, or one comma list ("A=1,B=1"), which is how powershell -File passes it.
    [string[]]$Switches = @("LICHII_COOP_PUPPET=1", "LICHII_COOP_OBJECTS=1", "LICHII_COOP_DEBUG_OBJECTS=1"),
    # Seconds to wait for each phase (boot and join, load, scenario).
    [int]$Timeout = 180,
    # Steps files: how long an `expect` line waits for the counts (seconds).
    [int]$ExpectWait = 30,
    # Skip load.txt (the games are expected to be in the world already: never, unless testing).
    [switch]$NoLoad,
    # Leave the games and server running at the end.
    [switch]$KeepOpen,
    # Copy the screen where each game is instead of asking the window to print itself, so the
    # co-op overlay windows on top (name tags, chat) are in the screenshots too.
    [switch]$ScreenCopy
)
$ErrorActionPreference = 'Stop'

function Resolve([string]$p) { [IO.Path]::GetFullPath($p) }
$here = Resolve $PSScriptRoot
$kitRoot = $null
for ($d = Get-Item $here; $d; $d = $d.Parent) {
    if (Test-Path (Join-Path $d.FullName 'coop-kit')) { $kitRoot = Join-Path $d.FullName 'coop-kit'; break }
}
if (-not $KitA) { if (-not $kitRoot) { throw "no coop-kit folder found; pass -KitA and -KitB" }; $KitA = "$kitRoot\auto-A" }
if (-not $KitB) { $KitB = "$(Split-Path (Resolve $KitA))\auto-B" }
$KitA = Resolve $KitA; $KitB = Resolve $KitB
if (-not $Game) {
    $Game = Get-ChildItem (Split-Path (Split-Path $KitA)) -Filter *.nup -File -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Game -or -not (Test-Path -LiteralPath $Game)) { throw "no game file; pass -Game" }
if (-not $Shots) { $Shots = Resolve "$(Split-Path $KitA)\shots" }
$scA = "$here\scenarios\$Scenario.A.txt"
$scB = "$here\scenarios\$Scenario.B.txt"
$steps = "$here\scenarios\$Scenario.steps.txt"
if (-not (Test-Path $steps)) { $steps = $null }
if (-not $steps -and -not (Test-Path $scA)) { throw "no scenario $scA" }
$Server = Resolve $Server
if (-not (Test-Path $Server)) { throw "no server at $Server; build it or pass -Server" }

$out = Join-Path $Shots "$Scenario\$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing;
public class AutotestWin {
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
 [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int ht, uint f);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
 public struct RECT { public int L,T,R,B; }
 public struct POINT { public int X,Y; }
 // Real pixels: without this a scaled desktop gives rects in scaled units and the copy
 // misses part of the window.
 public static void DpiAware() { SetProcessDPIAware(); }
 // Moves the window and keeps it above other windows, so a screen copy sees the game.
 public static void Place(IntPtr h, int x, int y, int w, int ht) { SetWindowPos(h, new IntPtr(-1), x, y, w, ht, 0x40); }
 // The game's client area only (no title bar or border). PrintWindow with
 // PW_CLIENTONLY|PW_RENDERFULLCONTENT first; if it gives a black picture (Vulkan),
 // copy the screen where the client area is instead.
 public static string Shot(IntPtr h, string path, bool screen) {
  RECT r; GetClientRect(h, out r);
  POINT o = new POINT(); ClientToScreen(h, ref o);
  int w = Math.Max(1, r.R - r.L), ht = Math.Max(1, r.B - r.T);
  using (var b = new Bitmap(w, ht)) {
   string how = "print";
   if (!screen) { using (var g = Graphics.FromImage(b)) { var dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); } }
   if (screen || Dark(b)) { using (var g = Graphics.FromImage(b)) g.CopyFromScreen(o.X, o.Y, 0, 0, new Size(w, ht)); how = "screen"; }
   int sw = Math.Max(1, w), sh = Math.Max(1, ht);
   using (var s = new Bitmap(b, sw, sh)) s.Save(path, System.Drawing.Imaging.ImageFormat.Png);
   return how;
  }
 }
 static bool Dark(Bitmap b) {
  for (int y = b.Height / 8; y < b.Height; y += Math.Max(1, b.Height / 8))
   for (int x = b.Width / 8; x < b.Width; x += Math.Max(1, b.Width / 8)) {
    var c = b.GetPixel(x, y); if (c.R + c.G + c.B > 30) return false; }
  return true;
 }
}
"@
[AutotestWin]::DpiAware()

$kits = [ordered]@{ A = $KitA; B = $KitB }
$st = @{}
foreach ($x in $kits.Keys) {
    $emu = Join-Path $kits[$x] 'emulator'
    $st[$x] = @{
        Emu = $emu
        Log = Join-Path $emu 'user\log\eden_log.txt'
        Script = Join-Path $kits[$x] 'autotest-script.txt'
        Proc = $null
        Done = 0
        Marks = 0
        Runs = 0
        Copied = $false
    }
}

function Read-Log($x) {
    $p = $st[$x].Log
    if (-not (Test-Path $p)) { return '' }
    $f = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite,Delete')
    try { (New-Object IO.StreamReader($f)).ReadToEnd() } finally { $f.Close() }
}
function Count($text, $pattern) { ([regex]::Matches($text, $pattern)).Count }

function Shot-Both([string]$name) {
    foreach ($x in $kits.Keys) {
        $p = $st[$x].Proc
        if ($p -and -not $p.HasExited) {
            $p.Refresh()
            [void][AutotestWin]::Shot($p.MainWindowHandle, (Join-Path $out "$name-$x.png"), [bool]$ScreenCopy)
        }
    }
}

# Plays $files (one script per game, $null for none) and waits until each game logs "done".
function Play([hashtable]$files, [string]$phase, [double]$every) {
    $want = @{}
    foreach ($x in $files.Keys) {
        if (-not $files[$x]) { continue }
        $st[$x].Done = Count (Read-Log $x) 'coop: test script done'
        $st[$x].Marks = Count (Read-Log $x) 'coop: test mark '
        # A comment with the time makes the file differ from the last one, so it is played again.
        Set-Content -Encoding ascii $st[$x].Script ("# $phase $(Get-Date -Format o)`r`n" + (Get-Content -Raw $files[$x]))
        $want[$x] = $true
    }
    $start = Get-Date; $next = $start; $n = 0
    while ($want.Count) {
        foreach ($x in @($want.Keys)) {
            if ($st[$x].Proc.HasExited) { throw "game $x closed during $phase" }
            $log = Read-Log $x
            $err = [regex]::Match($log, 'coop: test script [^\r\n]*: line [^\r\n]*')
            if ($err.Success) { throw "game ${x}: $($err.Value)" }
            $marks = [regex]::Matches($log, 'coop: test mark (\S+)')
            for ($i = $st[$x].Marks; $i -lt $marks.Count; $i++) { Shot-Both "$phase-$x-$($marks[$i].Groups[1].Value)" }
            $st[$x].Marks = $marks.Count
            if ((Count $log 'coop: test script done') -gt $st[$x].Done) { $want.Remove($x) }
        }
        if ($every -gt 0 -and (Get-Date) -ge $next) { Shot-Both ("{0}-{1:d3}" -f $phase, $n); $n++; $next = $next.AddSeconds($every) }
        if (((Get-Date) - $start).TotalSeconds -gt $Timeout) { throw "$phase did not finish in $Timeout s (still: $($want.Keys -join ', '))" }
        Start-Sleep -Milliseconds 250
    }
}

$serverProc = $null
$serverRuns = 0
$switchEnv = [ordered]@{}
# Starts the server (again); its output goes to server-out.txt, then server-out-2.txt, ...
function Start-Server {
    $script:serverRuns++
    $sfx = if ($script:serverRuns -gt 1) { "-$($script:serverRuns)" } else { '' }
    $script:serverProc = Start-Process $Server -ArgumentList "--port $Port" -PassThru -WindowStyle Minimized `
        -RedirectStandardOutput "$out\server-out$sfx.txt" -RedirectStandardError "$out\server-err$sfx.txt"
    Start-Sleep 1
}
function Stop-Server {
    if ($script:serverProc -and -not $script:serverProc.HasExited) { Stop-Process -Id $script:serverProc.Id -Force }
    Start-Sleep 1
}
# Starts one game. A log left by an earlier run of it in this test is kept as eden_log-<x>-<n>.txt.
function Start-Game([string]$x) {
    $s = $st[$x]
    if ($Exe -and -not $s.Copied) { Copy-Item $Exe (Join-Path $s.Emu 'lichii.exe') -Force; $s.Copied = $true }
    Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $s.Emu 'lichii.exe') } | Stop-Process -Force
    Set-Content -Encoding ascii (Join-Path $s.Emu 'user\config\coop.ini') "server = 127.0.0.1`r`nport = $Port`r`nname = $x"
    Set-Content -Encoding ascii $s.Script ''
    if (Test-Path $s.Log) {
        if ($s.Runs -gt 0) { Copy-Item $s.Log "$out\eden_log-$x-$($s.Runs).txt" }
        Remove-Item $s.Log -Force
    }
    $s.Runs++
    $old = @{}
    foreach ($k in $switchEnv.Keys) { $old[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $switchEnv[$k]) }
    $env:LICHII_COOP_TEST_SCRIPT = $s.Script
    $s.Proc = Start-Process (Join-Path $s.Emu 'lichii.exe') -ArgumentList "-g `"$Game`"" -WorkingDirectory $s.Emu -PassThru
    Remove-Item Env:LICHII_COOP_TEST_SCRIPT
    foreach ($k in $old.Keys) { [Environment]::SetEnvironmentVariable($k, $old[$k]) }
    $s.Done = 0; $s.Marks = 0
}
# Waits until the game has a window and has joined the server.
function Wait-Joined([string]$x) {
    $start = Get-Date
    while ($true) {
        $s = $st[$x]
        if ($s.Proc.HasExited) { throw "game $x closed while starting" }
        $s.Proc.Refresh()
        if ($s.Proc.MainWindowHandle -ne 0 -and (Read-Log $x) -match 'coop: welcomed as player') { break }
        if (((Get-Date) - $start).TotalSeconds -gt $Timeout) { throw "game $x did not join the server in $Timeout s" }
        Start-Sleep -Milliseconds 500
    }
}
# Side by side on the main screen, so a screen copy works too.
function Place-Windows {
    $i = 0
    foreach ($x in $kits.Keys) {
        $p = $st[$x].Proc
        if ($p -and -not $p.HasExited) {
            $p.Refresh()
            [void][AutotestWin]::ShowWindow($p.MainWindowHandle, 9)
            [AutotestWin]::Place($p.MainWindowHandle, 960 * $i, 0, 960, 600)
        }
        $i++
    }
}

# The puppets (CoopPuppet_*) and their fairies (Dm_Npc_Navi; the game's own Navi is Npc_Navi)
# alive in one game, from the objects debug lines: "coop: objects: + NAME 0x..." when the walk
# first sees an actor, "- NAME 0x..." when it is gone. Needs LICHII_COOP_DEBUG_OBJECTS=1.
function Live-Actors([string]$x) {
    $live = [ordered]@{}
    if (-not $st[$x].Proc -or $st[$x].Proc.HasExited) { return $live }
    $re = 'coop: objects: ([+-]) (CoopPuppet_\w+|Dm_Npc_Navi) (0x[0-9a-f]+)'
    foreach ($m in [regex]::Matches((Read-Log $x), $re)) {
        $key = $m.Groups[3].Value + ' ' + $m.Groups[2].Value
        if ($m.Groups[1].Value -eq '+') { $live[$key] = $m.Groups[2].Value } else { $live.Remove($key) }
    }
    $live
}
function Puppet-Count([string]$x) {
    $names = @((Live-Actors $x).Values)
    $p = @($names | Where-Object { $_ -like 'CoopPuppet_*' })
    $f = @($names | Where-Object { $_ -eq 'Dm_Npc_Navi' })
    $kinds = ($p | Group-Object | ForEach-Object { '{0} {1}' -f $_.Count, $_.Name }) -join ', '
    @{ Puppets = $p.Count; Fairies = $f.Count; Text = ('{0} puppet ({1}), {2} fairy' -f $p.Count, $kinds, $f.Count) }
}

# A steps file (scenarios\<name>.steps.txt) drives the whole test instead of the A/B scripts:
#   start A [B]           boot the games named (again), wait until they joined; fresh logs
#   load A [B]            play scenarios\load.txt in the games named (title to the save)
#   play A file [B file]  play scenarios\<file> in A (and B) and wait for both
#   close B / kill B      close game B's window (it leaves the server) / end it at once (a crash)
#   server stop / server start
#   sleep 10              seconds
#   expect NAME A=1 B=0   wait up to -ExpectWait s until each game named has that many puppets
#                         and as many fairies, then screenshot NAME; else FAIL (the test goes on)
#   shot NAME
# Results go to counts.txt beside the screenshots.
function Run-Steps([string]$file) {
    $n = 0
    foreach ($raw in Get-Content $file) {
        $n++
        $line = ($raw -replace '#.*$', '').Trim()
        if (-not $line) { continue }
        $w = @($line -split '\s+')
        $rest = @(if ($w.Count -gt 1) { $w[1..($w.Count - 1)] })
        Write-Host "step ${n}: $line"
        switch ($w[0]) {
            'start' {
                foreach ($x in $rest) { Start-Game $x; Start-Sleep 3 }
                foreach ($x in $rest) { Wait-Joined $x }
                Place-Windows
            }
            'load' {
                $f = @{}; foreach ($x in $rest) { $f[$x] = "$here\scenarios\load.txt" }
                Play $f "s$n-load" $Every
            }
            'play' {
                $f = @{}; for ($i = 0; $i + 1 -lt $rest.Count; $i += 2) { $f[$rest[$i]] = "$here\scenarios\$($rest[$i + 1])" }
                Play $f ("s$n-" + [IO.Path]::GetFileNameWithoutExtension($rest[1])) $Every
            }
            'close' {
                $p = $st[$rest[0]].Proc
                if ($p -and -not $p.HasExited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(20000)) { Stop-Process -Id $p.Id -Force } }
            }
            'kill' { $p = $st[$rest[0]].Proc; if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
            'server' { if ($rest[0] -eq 'stop') { Stop-Server } else { Start-Server } }
            'sleep' { Start-Sleep -Seconds ([double]$rest[0]) }
            'shot' { Shot-Both "s$n-$($rest[0])" }
            'expect' {
                $name = $rest[0]
                $want = [ordered]@{}
                foreach ($kv in $rest[1..($rest.Count - 1)]) { $k, $v = $kv -split '='; $want[$k] = [int]$v }
                $start = Get-Date
                while ($true) {
                    $ok = $true; $texts = @()
                    foreach ($x in $want.Keys) {
                        $c = Puppet-Count $x
                        $texts += "${x}: $($c.Text)"
                        if ($c.Puppets -ne $want[$x] -or $c.Fairies -ne $want[$x]) { $ok = $false }
                    }
                    if ($ok -or ((Get-Date) - $start).TotalSeconds -ge $ExpectWait) { break }
                    Start-Sleep -Milliseconds 500
                }
                $secs = [int]((Get-Date) - $start).TotalSeconds
                $res = '{0} {1} (want {2}, after {3} s): {4}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name,
                    (($rest[1..($rest.Count - 1)]) -join ' '), $secs, ($texts -join '; ')
                Write-Host $res
                Add-Content "$out\counts.txt" $res
                if (-not $ok) { $script:stepFails++ }
                Shot-Both "s$n-$name"
            }
            default { throw "steps line ${n}: unknown '$($w[0])'" }
        }
    }
}

$failed = $null
$stepFails = 0
try {
    Start-Server

    # powershell -File passes -Switches "A=1","B=1" as one string "A=1,B=1": split on commas.
    $Switches = @($Switches | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().Trim('"', "'") } | Where-Object { $_ })
    foreach ($e in $Switches) { if ($e -notmatch '^[A-Za-z_][A-Za-z0-9_]*=') { throw "switch '$e': want NAME=value" } }
    foreach ($e in $Switches) { $k, $v = $e -split '=', 2; Write-Host "switch $k=$v"; $switchEnv[$k] = $v }

    if ($steps) {
        Run-Steps $steps
        if ($stepFails) { Write-Host "$stepFails expect step(s) FAILED (counts.txt)" }
    } else {
        foreach ($x in $kits.Keys) { Start-Game $x; Start-Sleep 3 }
        # Wait for both windows and both joins.
        foreach ($x in $kits.Keys) { Wait-Joined $x }
        Place-Windows
        Write-Host "both joined"
        Shot-Both 'joined'

        if (-not $NoLoad) {
            $load = "$here\scenarios\load.txt"
            Play @{ A = $load; B = $load } 'load' $Every
            Shot-Both 'loaded'
            Write-Host "save loaded"
        }
        $files = @{ A = $scA; B = $(if (Test-Path $scB) { $scB } else { $null }) }
        Play $files $Scenario $Every
    }
    Start-Sleep 1
    Shot-Both 'end'
    Write-Host "scenario played"
} catch {
    $failed = $_.Exception.Message
    Write-Host "FAILED: $failed"
    try { Shot-Both 'failed' } catch {}
} finally {
    if (-not $KeepOpen) {
        foreach ($x in $kits.Keys) { $p = $st[$x].Proc; if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
        Stop-Server
    }
    foreach ($x in $kits.Keys) { if (Test-Path $st[$x].Log) { Copy-Item $st[$x].Log "$out\eden_log-$x.txt" } }
}
if ($failed) { Set-Content "$out\FAILED.txt" $failed }
$out
