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
    [string[]]$Switches = @("LICHII_COOP_PUPPET=1", "LICHII_COOP_OBJECTS=1", "LICHII_COOP_SPAWN=1", "LICHII_COOP_DEBUG_OBJECTS=1"),
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
    [switch]$ScreenCopy,
    # The game's saves (title folder in each NAND user folder). Before every boot the runner copies
    # each kit's baseline (<kit>\autotest-save\) back over the kit's saves, because the game
    # autosaves on every scene change and Continue loads the newest save: without this, a run
    # that walks into Kakariko makes the next run start there.
    [string]$TitleId = '0400486024DEC000',
    # Copy each kit's saves as they are now into <kit>\autotest-save\ (its baseline), then stop.
    [switch]$TakeBaseline,
    # Leave the saves as they are (the game continues from whatever it saved last).
    [switch]$NoRestore,
    # Co-op off: no server, and coop.ini names none (the log says "coop: off"), so nothing is
    # shared and the game is not patched for co-op unless a switch asks for it. The test script,
    # the object walk (LICHII_COOP_DEBUG_OBJECTS) and the objects cheat still run. Default
    # switches then: LICHII_COOP_DEBUG_OBJECTS=1 only. For telling a game behaviour from one of
    # ours (issue 125).
    [switch]$NoCoop,
    # Emulator add-ons to turn off for this run, by the name the emulator's Add-Ons list shows:
    # its built-in patches ("Dynamic FPS by Dark (built-in)", "Water ripples at any frame rate
    # (built-in)", "Max Resolution (built-in)") or a mod folder ("coop-puppet"). Written into each
    # kit's qt-config.ini [DisabledAddOns] before the boot; the old file is put back at the end. The
    # log then has no "Applying built-in patch" line for it. For telling the emulator's patches
    # or our puppet pack from the game (issue 125).
    [string[]]$AddOnsOff = @(),
    # The emulator's "30 FPS mode" (oot_30fps) for this run: the game steps twice per frame.
    [switch]$Fps30,
    # The server's join password (passed through an environment variable, not the command
    # line). Every session has one, and it encrypts the traffic. The games send it too unless a
    # steps file says `password A TEXT` (or `password A -` for none).
    [string]$ServerPassword = "autotest-pass",
    # A bad connection (docs/testing.md, "Bad connections"): tools\netsim\netsim.py relays each game
    # in -NetSimFor to the server, on port -Port+1 (A) or -Port+2 (B). -Lag is added round trip in
    # ms (half each way), -Jitter +- ms each way, -Loss % each way, -Spike "<ms>/<every s>" a stall
    # of both directions (rubber-banding). Off when all are 0/empty.
    [double]$Lag = 0,
    [double]$Jitter = 0,
    [double]$Loss = 0,
    [string]$Spike = "",
    # The games that go through the relay: A, B or both ("A,B"); the others connect straight.
    [string[]]$NetSimFor = @("A", "B"),
    # More relay options, as for netsim.py (for example "--dup 1", "--down loss=5" or "--seed 3");
    # "A:--rate 256" gives one to that game's relay only. Also turns the relay on alone.
    [string[]]$NetSimArgs = @(),
    # Python 3.8+ for the relay. Default: the recomp's .venv beside the repo, else py -3 / python.
    [string]$Python = "",
    # What runs the games: the patched emulator (kits A and B), or Hookshot, the static recomp
    # (docs/hookshot.md). Hookshot games are played through Hookshot's control bridge by
    # tools\autotest\hookshot.py: the same A/B scripts, shots and logs in the run folder.
    [ValidateSet('emulator', 'hookshot')][string]$Runtime = 'emulator',
    # Hookshot game folders (longshot.exe, or bin\longshot.exe, with hookshot_coop.dll and the
    # puppet pack in mods\). Default: hookshot-A and hookshot-B in the coop-kit folder. A folder's
    # autotest-save\ (a copy of its user\save\) is put back over user\save\ before each run.
    [string]$HookshotA = "",
    [string]$HookshotB = "",
    # Hookshot: the control pipes' name (\\.\pipe\<Pipe>-A and -B). Pairs that run at the same time
    # (tools\autotest\groups.py) each need their own, as they need their own -Port.
    [string]$Pipe = "coop-auto",
    # Hookshot: print the hookshot.py command and stop (nothing started, no run folder made).
    [switch]$DryRun
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
if ($Runtime -eq 'emulator' -and -not $TakeBaseline -and (-not $Game -or -not (Test-Path -LiteralPath $Game))) { throw "no game file; pass -Game" }
if (-not $Shots) { $Shots = Resolve "$(Split-Path $KitA)\shots" }
$scA = "$here\scenarios\$Scenario.A.txt"
$scB = "$here\scenarios\$Scenario.B.txt"
$steps = "$here\scenarios\$Scenario.steps.txt"
if (-not (Test-Path $steps)) { $steps = $null }
if (-not $TakeBaseline -and -not $steps -and -not (Test-Path $scA)) { throw "no scenario $scA" }

# Header lines in the A script (or the steps file), which the emulator reads as comments:
#   #! noload                  the script loads the save itself (as -NoLoad)
#   #! switches NAME=v,NAME=v  added to the switches (-Switches given on the command line win)
#   #! timeout 300             seconds for each phase (as -Timeout, which wins)
#   #! server ARGS             more coop_server arguments (stray-diag: --owner-hold-ms 0)
#   #! start REGEX             the game log must match REGEX once the save is loaded (default: a
#                              Graveyard object, the save point); checked after load.txt, or at
#                              the end for a script that loads itself
if ($NoCoop -and -not $PSBoundParameters.ContainsKey('Switches')) { $Switches = @('LICHII_COOP_DEBUG_OBJECTS=1') }
$headNoLoad = $false; $headSwitches = @(); $headStart = $null; $headServer = ''
$headFile = if ($steps) { $steps } elseif (Test-Path $scA) { $scA } else { $null }
if ($headFile) {
    foreach ($l in Get-Content $headFile) {
        if ($l -match '^#!\s*noload\s*$') { $headNoLoad = $true }
        elseif ($l -match '^#!\s*switches\s+(.+)$') { $headSwitches += @($Matches[1] -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
        elseif ($l -match '^#!\s*start\s+(.+)$') { $headStart = $Matches[1].Trim() }
        elseif ($l -match '^#!\s*server\s+(.+)$') { $headServer = $Matches[1].Trim() }
        elseif ($l -match '^#!\s*timeout\s+(\d+)\s*$' -and -not $PSBoundParameters.ContainsKey('Timeout')) { $Timeout = [int]$Matches[1] }
    }
}
if ($headNoLoad -and -not $NoLoad) { Write-Host "scenario header: noload"; $NoLoad = [switch]$true }
if ($headSwitches.Count) {
    $given = @($Switches | ForEach-Object { $_ -split ',' } | ForEach-Object { ($_.Trim() -split '=', 2)[0] })
    foreach ($e in $headSwitches) {
        $k = ($e -split '=', 2)[0]
        if (-not $PSBoundParameters.ContainsKey('Switches') -or $given -notcontains $k) { $Switches += $e }
    }
}
if (-not $headStart -and -not $NoLoad) { $headStart = 'coop: objects: \+ \w*Graveyard'; if (-not (@($Switches | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() }) -match '^LICHII_COOP_DEBUG_OBJECTS=')) { $Switches += 'LICHII_COOP_DEBUG_OBJECTS=1' } }

$Server = Resolve $Server
if (-not $TakeBaseline -and -not $NoCoop -and -not ($DryRun -and $Runtime -eq 'hookshot') -and -not (Test-Path $Server)) { throw "no server at $Server; build it or pass -Server" }

# Hookshot: tools\autotest\hookshot.py runs the whole test (server, both games, load, scenario,
# shots, logs) and leaves them in the run folder like the emulator run does.
if ($Runtime -eq 'hookshot') {
    # A steps file ($Scenario.steps.txt) is played by tools\autotest\hs_steps.py: the same verbs as
    # Run-Steps below (start, load, play, close, kill, server stop/start, password, try, expect, lines, shot).
    if ($TakeBaseline) { throw "-Runtime hookshot: copy <game>\user\save to <game>\autotest-save for a baseline" }
    foreach ($o in 'Exe', 'Game', 'ScreenCopy', 'Fps30', 'AddOnsOff', 'Lag', 'Jitter', 'Loss', 'Spike', 'NetSimArgs') {
        if ($PSBoundParameters.ContainsKey($o)) { throw "-$o is for the emulator; not with -Runtime hookshot" }
    }
    # The server's join password: always through the environment for the server (OOT_COOP_TEST_PASSWORD);
    # hs_steps.py gives it to every game without a 'password' line, and a hookshot.py that knows
    # --password (every session has one) gets it there.
    if ($ServerPassword) { $env:OOT_COOP_TEST_PASSWORD = $ServerPassword }
    if (-not $HookshotA) { if (-not $kitRoot) { throw "no coop-kit folder found; pass -HookshotA and -HookshotB" }; $HookshotA = "$kitRoot\hookshot-A" }
    if (-not $HookshotB) { $HookshotB = "$(Split-Path (Resolve $HookshotA))\hookshot-B" }
    if (-not $PSBoundParameters.ContainsKey('Shots')) { $Shots = Resolve "$(Split-Path (Resolve $HookshotA))\shots-hookshot" }
    $out = Join-Path $Shots "$Scenario\$(Get-Date -Format yyyyMMdd-HHmmss)"
    if (-not $DryRun) { New-Item -ItemType Directory -Force $out | Out-Null }
    $py = @()
    if ($Python) { $py = @($Python) } else {
        for ($d = Get-Item $here; $d; $d = $d.Parent) {
            $v = Join-Path $d.FullName 'oot-remake-recomp\.venv\Scripts\python.exe'
            if (Test-Path $v) { $py = @($v); break }
        }
        if (-not $py.Count) { if (Get-Command py -ErrorAction SilentlyContinue) { $py = @('py', '-3') } else { $py = @('python') } }
    }
    if ($steps) {
        if ($NoCoop) { throw "-NoCoop: a steps file needs the server" }
        $a = @($py | Select-Object -Skip 1) + @('-I', "$here\hs_steps.py", 'run', '--steps', $steps,
            '--game-a', (Resolve $HookshotA), '--game-b', (Resolve $HookshotB), '--out', $out,
            '--timeout', "$([Math]::Max($Timeout, 900))", '--expect-wait', "$ExpectWait")
        if ($NoRestore) { $a += '--no-restore' }
        if ($DryRun) { $a += '--dry-run' }
    } else {
        $a = @($py | Select-Object -Skip 1) + @('-I', "$here\hookshot.py", 'run', '--game-a', (Resolve $HookshotA),
            '--game-b', (Resolve $HookshotB), '--out', $out, '--name', $Scenario, '--script-a', $scA, '--every', "$Every",
            '--timeout', "$([Math]::Max($Timeout, 900))", '--scenario-timeout', "$([Math]::Max($Timeout, 600))")
        if (Test-Path $scB) { $a += @('--script-b', $scB) }
        if ($NoLoad) { $a += '--no-load' } else { $a += @('--load', "$here\scenarios\load.txt") }
        if ($NoRestore) { $a += '--no-restore' }
        if ($KeepOpen) { $a += '--keep-open' }
        if ($ServerPassword -and (Select-String -Quiet -SimpleMatch -Path "$here\hookshot.py" -Pattern '"--password"')) {
            $a += @('--password', $ServerPassword)
        }
    }
    if ($NoCoop) { $a += @('--env', 'HOS_COOP=0') } else { $a += @('--server', $Server, '--port', "$Port") }
    if ($Pipe -ne 'coop-auto') { $a += @('--pipe', $Pipe) }
    foreach ($e in @($Switches | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().Trim('"', "'") } | Where-Object { $_ })) {
        $a += @('--env', $e)
    }
    Write-Host "hookshot: $($py[0]) $($a -join ' ')"
    if ($DryRun -and $steps) { & $py[0] @a; return }   # hs_steps.py --dry-run prints the plan
    if ($DryRun) { Write-Host "hookshot: dry run, nothing started"; return }
    & $py[0] @a
    $code = $LASTEXITCODE
    $out
    if ($code) { exit $code }
    return
}
if ($DryRun -or $PSBoundParameters.ContainsKey('Pipe')) { throw "-DryRun and -Pipe are for -Runtime hookshot" }

# The folders a kit's emulator really uses: qt-config.ini can point nand/sdmc/load/dump anywhere
# (kits copied from another kit keep that kit's absolute paths).
function Kit-Dir([string]$kit, [string]$name) {
    $user = Join-Path $kit 'emulator\user'
    $ini = Join-Path $user 'config\qt-config.ini'
    if (Test-Path $ini) {
        $text = Get-Content -Raw $ini
        $isDefault = $text -match "(?m)^${name}_directory\\default=true\s*$"
        $m = [regex]::Match($text, "(?m)^${name}_directory=(.*?)\s*$")
        if (-not $isDefault -and $m.Success -and $m.Groups[1].Value) { return [IO.Path]::GetFullPath($m.Groups[1].Value) }
    }
    [IO.Path]::GetFullPath((Join-Path $user $name))
}
# Each NAND user folder that has this game's saves: <nand>\user\save\0000000000000000\<user>\<title>.
function Save-Folders([string]$kit) {
    $root = Join-Path (Kit-Dir $kit 'nand') 'user\save\0000000000000000'
    if (-not (Test-Path $root)) { return @() }
    @(Get-ChildItem $root -Directory | ForEach-Object { Join-Path $_.FullName $TitleId } | Where-Object { Test-Path $_ })
}
function Baseline-Dir([string]$kit) { Join-Path $kit 'autotest-save' }
# The baseline holds <user>\<title>\... for each user folder.
function Take-Baseline([string]$kit) {
    $base = Baseline-Dir $kit
    $folders = Save-Folders $kit
    if (-not $folders.Count) { throw "no saves of $TitleId under $(Kit-Dir $kit 'nand')" }
    if (Test-Path $base) { Remove-Item -Recurse -Force $base }
    foreach ($f in $folders) {
        $user = Split-Path -Leaf (Split-Path $f)
        New-Item -ItemType Directory -Force (Join-Path $base $user) | Out-Null
        Copy-Item -Recurse $f (Join-Path $base $user)
    }
    $newest = Get-ChildItem -Recurse $base -Filter progress.sav | Sort-Object LastWriteTime | Select-Object -Last 1
    Write-Host "baseline of $kit taken from $(Kit-Dir $kit 'nand'): $($folders.Count) user folder(s), newest $($newest.Directory.Name) $($newest.LastWriteTime)"
}
function Restore-Save([string]$kit) {
    $base = Baseline-Dir $kit
    if (-not (Test-Path $base)) {
        throw "no save baseline at $base. Load the kit once by hand (or check that its newest save is the Graveyard one), then run: tools\autotest\run.ps1 -TakeBaseline (or pass -NoRestore). docs/testing.md, 'Start place'"
    }
    $root = Join-Path (Kit-Dir $kit 'nand') 'user\save\0000000000000000'
    foreach ($u in Get-ChildItem $base -Directory) {
        $dst = Join-Path $root "$($u.Name)\$TitleId"
        if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item -Recurse (Join-Path $u.FullName $TitleId) (Split-Path $dst)
    }
}

$kitList = [ordered]@{ A = $KitA; B = $KitB }
$nandA = Kit-Dir $KitA 'nand'; $nandB = Kit-Dir $KitB 'nand'
foreach ($x in $kitList.Keys) {
    foreach ($n in 'nand', 'sdmc', 'load') {
        $d = Kit-Dir $kitList[$x] $n
        if (-not $d.StartsWith($kitList[$x] + '\', [StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "WARNING: kit $x uses $n folder $d (outside the kit). tools\autotest\kit-paths.ps1 -Fix points it back."
            if ($n -eq 'nand' -and -not $NoRestore) { throw "kit $x's NAND is outside the kit; the save restore would overwrite $d. Run kit-paths.ps1 -Fix or pass -NoRestore." }
        }
    }
}
if (-not $NoRestore -and $nandA -eq $nandB) {
    throw "kits A and B share one NAND ($nandA), so they share saves and autosaves. Run tools\autotest\kit-paths.ps1 -Fix (or pass -NoRestore)"
}
if ($TakeBaseline) {
    foreach ($x in $kitList.Keys) { Take-Baseline $kitList[$x] }
    return
}
if (-not $NoRestore) {
    foreach ($x in $kitList.Keys) {
        if (-not (Test-Path (Baseline-Dir $kitList[$x]))) {
            throw "no save baseline at $(Baseline-Dir $kitList[$x]). Check that the kit's newest save is the Graveyard one, then run: tools\autotest\run.ps1 -Scenario walk -TakeBaseline (or pass -NoRestore). docs/testing.md, 'Start place'"
        }
    }
}

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
 delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
 public struct RECT { public int L,T,R,B; }
 public struct POINT { public int X,Y; }
 // The visible top-level windows of one process, as "0xHANDLE 'title'", so the steps log
 // shows a dialog (such as "Are you sure you want to close Lichii?", title "Lichii") that
 // appeared beside the game window.
 public static string Windows(int pid) {
  var list = new System.Collections.Generic.List<string>();
  EnumWindows((h, l) => {
   uint p; GetWindowThreadProcessId(h, out p);
   if (p == (uint)pid && IsWindowVisible(h)) {
    var s = new System.Text.StringBuilder(256); GetWindowText(h, s, 256);
    list.Add(string.Format("0x{0:x} '{1}'", h.ToInt64(), s));
   }
   return true;
  }, IntPtr.Zero);
  return string.Join(", ", list.ToArray());
 }
 // The game's own window of process pid: the largest visible top-level window that is not the
 // co-op cheats window (with LICHII_COOP_CHEATS=1 that window can be the process's
 // MainWindowHandle, and the screenshots caught it instead of the game).
 public static IntPtr GameWindow(int pid) {
  IntPtr best = IntPtr.Zero; long bestArea = 0;
  EnumWindows((h, p) => {
   uint wp; GetWindowThreadProcessId(h, out wp);
   if (wp != (uint)pid || !IsWindowVisible(h)) return true;
   var s = new System.Text.StringBuilder(256); GetWindowText(h, s, 256);
   if (s.ToString().StartsWith("Co-op item cheats")) return true;
   RECT r; GetClientRect(h, out r);
   long area = (long)(r.R - r.L) * (r.B - r.T);
   if (area > bestArea) { bestArea = area; best = h; }
   return true;
  }, IntPtr.Zero);
  return best;
 }
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

# steps.txt beside the screenshots: what the driver did and when (local time to the ms), and
# each game's process and windows, so a game that ends by itself can be matched to the logs
# (issue 112: game A's frontend shut down 0.2 s after B rejoined, cause not known).
function Note([string]$text) {
    $line = "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $text
    Add-Content "$out\steps.txt" $line
}
function Note-Games([string]$when) {
    foreach ($x in $kits.Keys) {
        $p = $st[$x].Proc
        if (-not $p) { Note "$when ${x}: not started"; continue }
        if ($p.HasExited) { Note "$when ${x}: pid $($p.Id) exited (code $($p.ExitCode))"; continue }
        $p.Refresh()
        Note ("$when ${x}: pid $($p.Id) main 0x{0:x} windows: {1}" -f [int64]$p.MainWindowHandle, [AutotestWin]::Windows($p.Id))
    }
}
# Games the driver did not close or kill but that are gone: logged with the step, and they
# fail the run (a game that quits on its own is a bug, whoever asked it to).
$expectedExit = @{}
$passwords = @{}
function Check-Exits([string]$step) {
    foreach ($x in $kits.Keys) {
        $p = $st[$x].Proc
        if ($p -and $p.HasExited -and -not $expectedExit[$x]) {
            $expectedExit[$x] = $true
            $msg = "game $x ended by itself during $step (exit code $($p.ExitCode)); its log: eden_log-$x*.txt, look for 'Shutdown requested'"
            Write-Host $msg; Note $msg
            Add-Content "$out\counts.txt" "FAIL $msg"
            $script:stepFails++
        }
    }
}

# The game window of game x (not the cheats window), or 0.
function Game-Window([string]$x) {
    $p = $st[$x].Proc
    if (-not $p -or $p.HasExited) { return [IntPtr]::Zero }
    $h = [AutotestWin]::GameWindow($p.Id)
    if ($h -eq [IntPtr]::Zero) { $p.Refresh(); $h = $p.MainWindowHandle }
    $h
}
function Shot-Both([string]$name) {
    foreach ($x in $kits.Keys) {
        $h = Game-Window $x
        if ($h -ne [IntPtr]::Zero) {
            [void][AutotestWin]::Shot($h, (Join-Path $out "$name-$x.png"), [bool]$ScreenCopy)
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
    if ($NoCoop) { return }
    $script:serverRuns++
    $sfx = if ($script:serverRuns -gt 1) { "-$($script:serverRuns)" } else { '' }
    $sargs = "--port $Port"
    if (-not $ServerPassword) { throw "-ServerPassword: every session has a join password (coop_server does not start without one)" }
    $env:OOT_COOP_TEST_PASSWORD = $ServerPassword; $sargs += " --password-env OOT_COOP_TEST_PASSWORD"
    if ($headServer) { $sargs += " $headServer" }
    $script:serverProc = Start-Process $Server -ArgumentList $sargs -PassThru -WindowStyle Minimized `
        -RedirectStandardOutput "$out\server-out$sfx.txt" -RedirectStandardError "$out\server-err$sfx.txt"
    Start-Sleep 1
}
function Stop-Server {
    if ($script:serverProc -and -not $script:serverProc.HasExited) { Stop-Process -Id $script:serverProc.Id -Force }
    Start-Sleep 1
}
# The kits' emulators ask "Are you sure you want to close Lichii?" when their window is closed
# (confirmStop=0, Ask_Always). Nobody answers it in a test, so `close B` waited 20 s and then
# ended B like a crash, and the question stayed open until then (issue 112). Ask_Never (2) makes
# `close` a real leave. Only the [UI] key changes; the rest of qt-config.ini is kept.
function Set-NoCloseQuestion([string]$ini) {
    if (-not (Test-Path $ini)) { return }
    $text = [IO.File]::ReadAllText($ini)
    $new = $text -replace '(?m)^confirmStop\\default=.*$', 'confirmStop\default=false' -replace '(?m)^confirmStop=.*$', 'confirmStop=2'
    if ($new -ne $text) { [IO.File]::WriteAllText($ini, $new) }
}
# -AddOnsOff and -Fps30: this run's [DisabledAddOns] entry for the game and its 30 FPS mode. The
# kit's own file is kept in $iniKept (once per kit) and put back in the finally block.
$iniKept = @{}
function Set-RunSettings([string]$x, [string]$ini) {
    if (-not $AddOnsOff.Count -and -not $Fps30) { return }
    if (-not (Test-Path $ini)) { throw "no $ini for -AddOnsOff/-Fps30" }
    $text = [IO.File]::ReadAllText($ini)
    if (-not $iniKept.ContainsKey($x)) { $iniKept[$x] = @{ Path = $ini; Text = $text } }
    $nl = if ($text -match "`r`n") { "`r`n" } else { "`n" }
    if ($AddOnsOff.Count) {
        $title = [Convert]::ToUInt64($TitleId, 16)
        # Qt's ini array layout, as the emulator writes it (frontend_common/config.cpp,
        # SaveDisabledAddOnValues). Names are quoted so a comma stays part of the name.
        $lines = @('[DisabledAddOns]', 'size=1', '1\title_id\default=false', "1\title_id=$title", "1\disabled\size=$($AddOnsOff.Count)")
        for ($i = 0; $i -lt $AddOnsOff.Count; $i++) {
            if ($AddOnsOff[$i] -match '["\\]') { throw "-AddOnsOff '$($AddOnsOff[$i])': no quotes or backslashes" }
            $lines += "1\disabled\$($i + 1)\d\default=false"
            $lines += "1\disabled\$($i + 1)\d=`"$($AddOnsOff[$i])`""
        }
        $section = ($lines -join $nl) + $nl
        $rx = [regex]'(?ms)^\[DisabledAddOns\]\r?\n.*?(?=^\[|\z)'
        $text = if ($rx.IsMatch($text)) { $rx.Replace($text, ($section + $nl).Replace('$', '$$'), 1) } else { $section + $nl + $text }
    }
    if ($Fps30) {
        if ($text -notmatch '(?m)^oot_30fps=') { throw "$ini has no oot_30fps key (an older emulator?)" }
        $text = $text -replace '(?m)^oot_30fps\\default=.*$', 'oot_30fps\default=false' -replace '(?m)^oot_30fps=.*$', 'oot_30fps=true'
    }
    [IO.File]::WriteAllText($ini, $text)
    Write-Host "game ${x}: add-ons off: $(if ($AddOnsOff.Count) { $AddOnsOff -join ' | ' } else { 'none' }); 30 FPS mode: $([bool]$Fps30)"
}
function Restore-RunSettings {
    foreach ($k in @($iniKept.Keys)) {
        for ($t = 0; ; $t++) {
            try { [IO.File]::WriteAllText($iniKept[$k].Path, $iniKept[$k].Text); break }
            catch { if ($t -ge 20) { Write-Host "could not put back $($iniKept[$k].Path): $_"; break }; Start-Sleep -Milliseconds 500 }
        }
    }
}
# Starts one game. A log left by an earlier run of it in this test is kept as eden_log-<x>-<n>.txt.
function Start-Game([string]$x) {
    $s = $st[$x]
    if ($Exe -and -not $s.Copied) { Copy-Item $Exe (Join-Path $s.Emu 'lichii.exe') -Force; $s.Copied = $true }
    $left = @(Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $s.Emu 'lichii.exe') })
    $left | Stop-Process -Force
    $left | ForEach-Object { [void]$_.WaitForExit(15000) }
    if (-not $NoRestore) {
        # A game just ended may hold its save files a moment longer.
        for ($t = 0; ; $t++) {
            try { Restore-Save $kits[$x]; break }
            catch { if ($t -ge 20 -or $_.Exception.Message -like 'no save baseline*') { throw }; Start-Sleep -Milliseconds 500 }
        }
        Write-Host "game ${x}: saves restored from $(Baseline-Dir $kits[$x])"
    }
    $ini = if ($NoCoop) { "name = $x" } else { "server = 127.0.0.1`r`nport = $($netPort[$x])`r`nname = $x" }
    $pw = if ($script:passwords.ContainsKey($x)) { $script:passwords[$x] } else { $ServerPassword }
    if (-not $NoCoop -and $pw) { $ini += "`r`npassword = $pw" }
    Set-Content -Encoding ascii (Join-Path $s.Emu 'user\config\coop.ini') $ini
    Set-NoCloseQuestion (Join-Path $s.Emu 'user\config\qt-config.ini')
    Set-RunSettings $x (Join-Path $s.Emu 'user\config\qt-config.ini')
    Set-Content -Encoding ascii $s.Script ''
    if (Test-Path $s.Log) {
        if ($s.Runs -gt 0) { Copy-Item $s.Log "$out\eden_log-$x-$($s.Runs).txt" }
        # A game just ended may hold its log a moment longer.
        for ($t = 0; ; $t++) {
            try { Remove-Item $s.Log -Force -ErrorAction Stop; break }
            catch { if ($t -ge 40) { throw }; Start-Sleep -Milliseconds 250 }
        }
    }
    $s.Runs++
    $old = @{}
    # Plain NAME first, then A:NAME / B:NAME, so a per-game value wins; the old value is
    # saved once, before the first set.
    foreach ($key in @($switchEnv.Keys | Sort-Object { $_ -match '^[AB]:' })) {
        $k = $key
        if ($key -match '^([AB]):(.+)$') { if ($Matches[1] -ne $x) { continue }; $k = $Matches[2] }
        if (-not $old.ContainsKey($k)) { $old[$k] = [Environment]::GetEnvironmentVariable($k) }
        [Environment]::SetEnvironmentVariable($k, $switchEnv[$key])
    }
    if ($NoCoop) {
        # No server from the environment either.
        if (-not $old.ContainsKey('LICHII_COOP_SERVER')) { $old['LICHII_COOP_SERVER'] = $env:LICHII_COOP_SERVER }
        Remove-Item Env:LICHII_COOP_SERVER -ErrorAction SilentlyContinue
    }
    $env:LICHII_COOP_TEST_SCRIPT = $s.Script
    $s.Proc = Start-Process (Join-Path $s.Emu 'lichii.exe') -ArgumentList "-g `"$Game`"" -WorkingDirectory $s.Emu -PassThru
    Remove-Item Env:LICHII_COOP_TEST_SCRIPT
    foreach ($k in $old.Keys) { [Environment]::SetEnvironmentVariable($k, $old[$k]) }
    $s.Done = 0; $s.Marks = 0
    $expectedExit[$x] = $false
    Note "started ${x}: pid $($s.Proc.Id)"
}
# Waits until the game has a window and has joined the server (with -NoCoop: has said
# "coop: off").
function Wait-Joined([string]$x) {
    $joined = if ($NoCoop) { 'coop: off' } else { 'coop: welcomed as player' }
    $start = Get-Date
    while ($true) {
        $s = $st[$x]
        if ($s.Proc.HasExited) { throw "game $x closed while starting" }
        $s.Proc.Refresh()
        if ($s.Proc.MainWindowHandle -ne 0 -and (Read-Log $x) -match $joined) { break }
        if (((Get-Date) - $start).TotalSeconds -gt $Timeout) { throw "game $x did not reach '$joined' in $Timeout s" }
        Start-Sleep -Milliseconds 500
    }
}
# Side by side on the main screen, so a screen copy works too.
function Place-Windows {
    $i = 0
    foreach ($x in $kits.Keys) {
        $h = Game-Window $x
        if ($h -ne [IntPtr]::Zero) {
            [void][AutotestWin]::ShowWindow($h, 9)
            [AutotestWin]::Place($h, 960 * $i, 0, 960, 600)
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
# Did game x get to the scenario's start place (the `#! start` regex, default a Graveyard object
# after load.txt)? Waits up to $wait s; writes start.txt beside the screenshots.
function Check-Start([string]$x, [int]$wait) {
    if (-not $headStart) { return $true }
    $start = Get-Date
    while (-not ((Read-Log $x) -match $headStart)) {
        if (((Get-Date) - $start).TotalSeconds -ge $wait) {
            Add-Content "$out\start.txt" "FAIL ${x}: no '$headStart' in the log (it loaded somewhere else, or LICHII_COOP_DEBUG_OBJECTS is off)"
            return $false
        }
        Start-Sleep -Milliseconds 500
    }
    Add-Content "$out\start.txt" "PASS ${x}: '$headStart'"
    $true
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
#   password B TEXT       the join password game B sends from its next start (- for none)
#   try B welcomed        start B (no wait for the join) and check the outcome within 40 s:
#                         welcomed (its log), refused (a new "refused a connection" line in the
#                         server log) or none (neither: turned away at connect); then end B
#   lines NAME B max=N REGEX  game B's log has at most N lines matching REGEX (the rest of the
#                         line, spaces kept); PASS or FAIL with the count (a loop, a repeat);
#                         min=N instead: at least N (the case really happened)
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
        Note "step ${n}: $line"
        switch ($w[0]) {
            'start' {
                foreach ($x in $rest) { Start-Game $x; Start-Sleep 3 }
                foreach ($x in $rest) { Wait-Joined $x }
                Place-Windows
            }
            'load' {
                $f = @{}; foreach ($x in $rest) { $f[$x] = "$here\scenarios\load.txt" }
                Play $f "s$n-load" $Every
                foreach ($x in $rest) { if (-not (Check-Start $x 15)) { $script:stepFails++ } }
            }
            'play' {
                $f = @{}; for ($i = 0; $i + 1 -lt $rest.Count; $i += 2) { $f[$rest[$i]] = "$here\scenarios\$($rest[$i + 1])" }
                Play $f ("s$n-" + [IO.Path]::GetFileNameWithoutExtension($rest[1])) $Every
            }
            'close' {
                $p = $st[$rest[0]].Proc
                $expectedExit[$rest[0]] = $true
                if ($p -and -not $p.HasExited) {
                    $p.Refresh()
                    Note ("close $($rest[0]): pid $($p.Id) main window 0x{0:x}" -f [int64]$p.MainWindowHandle)
                    [void]$p.CloseMainWindow()
                    if (-not $p.WaitForExit(20000)) {
                        $msg = "game $($rest[0]) did not close in 20 s: ended. Its windows: $([AutotestWin]::Windows($p.Id))"
                        Write-Host $msg; Note $msg
                        Stop-Process -Id $p.Id -Force; [void]$p.WaitForExit(15000)
                    }
                }
            }
            'kill' { $expectedExit[$rest[0]] = $true; $p = $st[$rest[0]].Proc; if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force; [void]$p.WaitForExit(15000) } }
            'server' { if ($rest[0] -eq 'stop') { Stop-Server } else { Start-Server } }
            'sleep' { Start-Sleep -Seconds ([double]$rest[0]) }
            'password' { $script:passwords[$rest[0]] = if ($rest[1] -eq '-') { '' } else { $rest[1] } }
            'try' {
                $x = $rest[0]; $want = $rest[1]
                $refusedBefore = @(Get-Content "$out\server-*.txt" -ErrorAction SilentlyContinue | Select-String 'refused a connection').Count
                Start-Game $x
                $start = Get-Date; $got = 'none'
                while (((Get-Date) - $start).TotalSeconds -lt 40) {
                    if ((Read-Log $x) -match 'coop: welcomed as player') { $got = 'welcomed'; break }
                    $n2 = @(Get-Content "$out\server-*.txt" -ErrorAction SilentlyContinue | Select-String 'refused a connection').Count
                    if ($n2 -gt $refusedBefore) { $got = 'refused'; break }
                    Start-Sleep -Milliseconds 500
                }
                Start-Sleep 3
                Shot-Both "s$n-try-$x-$want"
                $res = '{0} try {1} (want {2}, got {3}, password set: {4})' -f $(if ($got -eq $want) { 'PASS' } else { 'FAIL' }), $x, $want, $got, [bool]$(if ($script:passwords.ContainsKey($x)) { $script:passwords[$x] } else { $ServerPassword })
                Write-Host $res; Add-Content "$out\counts.txt" $res
                if ($got -ne $want) { $script:stepFails++ }
                $expectedExit[$x] = $true; $p = $st[$x].Proc
                if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force; [void]$p.WaitForExit(15000) }
                Copy-Item $st[$x].Log "$out\eden_log-$x-try$n.txt" -ErrorAction SilentlyContinue
            }
            'shot' { Shot-Both "s$n-$($rest[0])" }
            'lines' {
                if ($rest.Count -lt 4 -or $rest[2] -notmatch '^(max|min)=(\d+)$') { throw "steps line ${n}: want lines NAME B max=N|min=N REGEX" }
                $atLeast = $Matches[1] -eq 'min'
                $bound = [int]$Matches[2]
                $pattern = ($line -split '\s+', 5)[4]
                $got = Count (Read-Log $rest[1]) "(?m)^.*$pattern"
                $ok = if ($atLeast) { $got -ge $bound } else { $got -le $bound }
                $res = '{0} {1} ({2}: {3} lines match ''{4}'', {5} {6})' -f $(if ($ok) { 'PASS' } else { 'FAIL' }),
                    $rest[0], $rest[1], $got, $pattern, $(if ($atLeast) { 'at least' } else { 'at most' }), $bound
                Write-Host $res
                Add-Content "$out\counts.txt" $res
                if (-not $ok) { $script:stepFails++ }
            }
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
        Note-Games "after step ${n}"
        Check-Exits "step ${n} ($line)"
    }
}

# The bad-connection relays (one per game in -NetSimFor), started once with the first server and
# kept through server restarts. $netPort[x] is the port that game's coop.ini names.
$netProcs = @{}
$netPort = @{ A = $Port; B = $Port }
$NetSimFor = @($NetSimFor | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToUpper() } | Where-Object { $_ })
$NetSimArgs = @($NetSimArgs | Where-Object { $_ })
$netOn = (-not $NoCoop) -and ($Lag -or $Jitter -or $Loss -or $Spike -or $NetSimArgs.Count) -and $NetSimFor.Count
function Find-Python {
    if ($Python) { return @($Python) }
    for ($d = Get-Item $here; $d; $d = $d.Parent) {
        $v = Join-Path $d.FullName 'oot-remake-recomp\.venv\Scripts\python.exe'
        if (Test-Path $v) { return @($v) }
    }
    if (Get-Command py -ErrorAction SilentlyContinue) { return @('py', '-3') }
    if (Get-Command python -ErrorAction SilentlyContinue) { return @('python') }
    throw "no Python for the relay (tools\netsim\netsim.py); pass -Python"
}
function Start-NetSim {
    if (-not $netOn) { return }
    $py = @(Find-Python)
    $relay = Resolve "$here\..\netsim\netsim.py"
    $lines = @("netsim: lag $Lag ms (round trip), jitter +-$Jitter ms, loss $Loss %, spike '$Spike', for $($NetSimFor -join ','), extra '$($NetSimArgs -join ' ')'")
    foreach ($x in $NetSimFor) {
        if ($x -notin @('A', 'B')) { throw "-NetSimFor: '$x' is not A or B" }
        $p = if ($x -eq 'A') { $Port + 1 } else { $Port + 2 }
        $a = @($py | Select-Object -Skip 1) + @("`"$relay`"", '--listen', "127.0.0.1:$p", '--server', "127.0.0.1:$Port",
            '--name', $x, '--report-every', '10',
            '--lag', "$Lag", '--jitter', "$Jitter", '--loss', "$Loss")
        if ($Spike) { $a += @('--spike', $Spike) }
        foreach ($e in $NetSimArgs) {
            if ($e -match '^([AB]):(.*)$') { if ($Matches[1] -eq $x) { $a += $Matches[2] } } else { $a += $e }
        }
        $proc = Start-Process $py[0] -ArgumentList ($a -join ' ') -PassThru -WindowStyle Minimized `
            -RedirectStandardOutput "$out\netsim-$x.txt" -RedirectStandardError "$out\netsim-err-$x.txt"
        $script:netProcs[$x] = $proc
        $script:netPort[$x] = $p
        $lines += "game ${x}: relay 127.0.0.1:$p -> server 127.0.0.1:$Port ($($a -join ' '))"
    }
    Start-Sleep 1
    foreach ($x in $netProcs.Keys) {
        if ($netProcs[$x].HasExited) { throw "the relay for $x stopped: $(Get-Content "$out\netsim-err-$x.txt" -Raw)" }
    }
    $lines | ForEach-Object { Write-Host $_ }
    Set-Content -Encoding utf8 "$out\netsim.txt" $lines
}
function Stop-NetSim {
    foreach ($p in $script:netProcs.Values) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
}

$failed = $null
$stepFails = 0
try {
    Start-Server
    Start-NetSim

    # powershell -File passes -Switches "A=1","B=1" as one string "A=1,B=1": split on commas.
    $Switches = @($Switches | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().Trim('"', "'") } | Where-Object { $_ })
    # A:NAME=value or B:NAME=value goes to that game only (for example a key set in A alone).
    foreach ($e in $Switches) { if ($e -notmatch '^([AB]:)?[A-Za-z_][A-Za-z0-9_]*=') { throw "switch '$e': want NAME=value or A:NAME=value" } }
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
            foreach ($x in $kits.Keys) {
                if (-not (Check-Start $x 15)) { throw "game $x did not start at the save point ('$headStart'): see start.txt" }
            }
        }
        $files = @{ A = $scA; B = $(if (Test-Path $scB) { $scB } else { $null }) }
        Play $files $Scenario $Every
        if ($NoLoad -and $headStart) {
            foreach ($x in $kits.Keys) { if (-not (Check-Start $x 0)) { $stepFails++ } }
        }
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
        foreach ($x in $kits.Keys) { $expectedExit[$x] = $true; $p = $st[$x].Proc; if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
        Stop-Server
        Stop-NetSim
    }
    foreach ($x in $kits.Keys) { if (Test-Path $st[$x].Log) { Copy-Item $st[$x].Log "$out\eden_log-$x.txt" } }
    if (-not $KeepOpen) {
        foreach ($x in $kits.Keys) { $p = $st[$x].Proc; if ($p) { [void]$p.WaitForExit(15000) } }
        Restore-RunSettings
    } elseif ($iniKept.Count) {
        Write-Host "-KeepOpen: the kits keep this run's add-ons/30 FPS settings; their own qt-config.ini is not put back"
    }
}
if (-not $failed -and $stepFails) { $failed = "$stepFails expect step(s) failed: see counts.txt" }
if ($failed) { Set-Content "$out\FAILED.txt" $failed }
$out
