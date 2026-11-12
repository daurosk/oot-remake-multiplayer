# A mixed session (gate G8): one emulator game and a Hookshot pair on ONE server.
#
#   tools\autotest\mixed.ps1                      # scenario "mixed": mixed-emu + mixed-hs
#   tools\autotest\mixed.ps1 -Scenario mixed -KeepOpen
#
# Starts the server (-Server, default <coop-kit>\auto-A\server\coop_server.exe: a path that ran
# before, so no firewall prompt), then at the same time:
#   - run.ps1 -NoServer -Scenario <Scenario>-emu: the emulator kit -KitA alone (steps file: start A,
#     load A, wait until both Hookshot players are in its world, then chat, give, walk, lines);
#   - run.ps1 -NoServer -Runtime hookshot -Scenario <Scenario>-hs: the Hookshot pair (HookA, HookB).
# Both runs keep their own folders (emu\ and hs\ in the run folder). At the end this script grades
# the logs into checks.txt (PASS / FAIL per row) and writes FAILED.txt when a row failed.
# One run at a time on a PC (TEST-TURN); three games at once.
param(
    [string]$Scenario = 'mixed',
    [string]$KitA = '',
    [string]$HookshotA = '',
    [string]$HookshotB = '',
    [string]$Server = '',
    [int]$Port = 27560,
    [string]$Shots = '',
    # Seconds the emulator waits for the Hookshot players (a pair boots and loads in about 4 min).
    [int]$ExpectWait = 600,
    [switch]$KeepOpen,
    [string]$Python = '',
    # Print what would run and stop.
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
function Resolve([string]$p) { [IO.Path]::GetFullPath($p) }
$here = Resolve $PSScriptRoot
$kitRoot = $null
for ($d = Get-Item $here; $d; $d = $d.Parent) {
    if (Test-Path (Join-Path $d.FullName 'coop-kit')) { $kitRoot = Join-Path $d.FullName 'coop-kit'; break }
}
if (-not $kitRoot -and (-not $KitA -or -not $HookshotA -or -not $Server)) { throw "no coop-kit folder found; pass -KitA, -HookshotA, -HookshotB and -Server" }
if (-not $KitA) { $KitA = "$kitRoot\auto-A" }
if (-not $HookshotA) { $HookshotA = "$kitRoot\hookshot-A" }
if (-not $HookshotB) { $HookshotB = "$(Split-Path (Resolve $HookshotA))\hookshot-B" }
if (-not $Server) { $Server = "$kitRoot\auto-A\server\coop_server.exe" }
$KitA = Resolve $KitA; $HookshotA = Resolve $HookshotA; $HookshotB = Resolve $HookshotB; $Server = Resolve $Server
if (-not $Shots) { $Shots = Resolve "$(Split-Path $KitA)\shots-mixed" }
foreach ($f in "$here\scenarios\$Scenario-emu.steps.txt", "$here\scenarios\$Scenario-hs.A.txt") {
    if (-not (Test-Path $f)) { throw "no scenario file $f" }
}
if (-not (Test-Path $Server)) { throw "no server at $Server" }
$out = Join-Path $Shots "$Scenario\$(Get-Date -Format yyyyMMdd-HHmmss)"
$run = "$here\run.ps1"
$emuArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $run, '-Scenario', "$Scenario-emu", '-NoServer', '-Port', "$Port",
    '-KitA', $KitA, '-Shots', "$out\emu", '-ExpectWait', "$ExpectWait")
$hsArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $run, '-Scenario', "$Scenario-hs", '-Runtime', 'hookshot', '-NoServer',
    '-Port', "$Port", '-HookshotA', $HookshotA, '-HookshotB', $HookshotB, '-Shots', "$out\hs")
if ($KeepOpen) { $emuArgs += '-KeepOpen'; $hsArgs += '-KeepOpen' }
if ($Python) { $emuArgs += @('-Python', $Python); $hsArgs += @('-Python', $Python) }
Write-Host "mixed: server $Server --port $Port"
Write-Host "mixed: emulator: powershell $($emuArgs -join ' ')"
Write-Host "mixed: hookshot: powershell $($hsArgs -join ' ')"
if ($DryRun) { Write-Host 'mixed: dry run, nothing started'; return }
New-Item -ItemType Directory -Force $out | Out-Null

# Start-Process joins an argument list with spaces and quotes nothing: the kit paths have spaces.
function Quote([string[]]$list) { ($list | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ' }
function Note([string]$t) { Add-Content "$out\steps.txt" ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $t) }
$server = Start-Process $Server -ArgumentList "--port $Port" -PassThru -WindowStyle Minimized `
    -RedirectStandardOutput "$out\server-out.txt" -RedirectStandardError "$out\server-err.txt"
Note "server pid $($server.Id) on port $Port"
Start-Sleep 1
$emu = $null; $hs = $null
try {
    if ($server.HasExited) { throw "the server stopped: $(Get-Content -Raw "$out\server-err.txt")" }
    $emu = Start-Process powershell -ArgumentList (Quote $emuArgs) -PassThru -WindowStyle Minimized `
        -RedirectStandardOutput "$out\emu-out.txt" -RedirectStandardError "$out\emu-err.txt"
    Note "emulator run pid $($emu.Id)"
    $hs = Start-Process powershell -ArgumentList (Quote $hsArgs) -PassThru -WindowStyle Minimized `
        -RedirectStandardOutput "$out\hs-out.txt" -RedirectStandardError "$out\hs-err.txt"
    Note "hookshot run pid $($hs.Id)"
    $emu.WaitForExit(); Note "emulator run ended ($($emu.ExitCode))"
    $hs.WaitForExit(); Note "hookshot run ended ($($hs.ExitCode))"
} finally {
    if (-not $KeepOpen -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}

# Grading: one row per check, from the logs both runs copied into their folders.
function Newest([string]$dir) { Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1 }
function Text([string]$p) { if ($p -and (Test-Path $p)) { [IO.File]::ReadAllText($p) } else { '' } }
function Count([string]$text, [string]$re) { ([regex]::Matches($text, $re)).Count }
$emuDir = Newest "$out\emu\$Scenario-emu"
$hsDir = Newest "$out\hs\$Scenario-hs"
$emuLog = if ($emuDir) { Text (Join-Path $emuDir.FullName 'eden_log-A.txt') } else { '' }
$hsLog = @{}
foreach ($x in 'A', 'B') {
    $hsLog[$x] = ''
    if ($hsDir) {
        foreach ($n in "hookshot_log-$x.txt", "game-out-$x.txt") { $hsLog[$x] += Text (Join-Path $hsDir.FullName $n) }
    }
}
$srv = Text "$out\server-out.txt"
$rows = @()
function Row([string]$name, [bool]$ok, [string]$why) { $script:rows += ('{0} {1}: {2}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $why) }
# Distinct actors of that kind ever made in a log (by address).
function Made([string]$text, [string]$re) {
    $set = @{}
    foreach ($m in [regex]::Matches($text, "coop: objects: \+ ($re) (0x[0-9a-f]+)")) { $set[$m.Groups[2].Value] = 1 }
    $set.Count
}
$joined = Count $srv 'player \d+ joined'
Row 'server-3-joined' ($joined -ge 3) "$joined 'player N joined' lines (want A, HookA, HookB)"
Row 'emu-welcomed' ((Count $emuLog 'coop: welcomed as player') -ge 1) 'emulator log: coop: welcomed as player'
foreach ($x in 'A', 'B') { Row "hook$x-welcomed" ((Count $hsLog[$x] 'coop: welcomed as player') -ge 1) "Hook$x log: coop: welcomed as player" }
$emuIn = Count $emuLog 'coop: objects: \+ \w*Graveyard'
Row 'emu-in-world' ($emuIn -ge 1) "emulator log: $emuIn Graveyard objects (in the world, not stuck at the load)"
$retries = Text (Join-Path "$($emuDir.FullName)" 'load-retries.txt')
if ($retries) { $rows += "NOTE emu-load: extra A presses: $(($retries -split "`r?`n" | Where-Object { $_ }) -join ', ')" }
$p = Made $emuLog 'CoopPuppet_\w+'; $f = Made $emuLog 'Dm_Npc_Navi'
Row 'emu-sees-hookshot-as-link' ($p -ge 2) "emulator made $p CoopPuppet_* and $f puppet fairies (want 2: HookA, HookB)"
foreach ($x in 'A', 'B') {
    $p = Made $hsLog[$x] 'CoopPuppet_\w+'; $f = Made $hsLog[$x] 'Dm_Npc_Navi'
    Row "hook$x-sees-others" (($p + $f) -ge 2) "Hook$x made $p CoopPuppet_* and $f fairies for the other two (Link puppets need hs-puppets-b; fairies only before it)"
}
Row 'chat-emu-sent' ((Count $emuLog "coop: test chat 'hello from the emulator': sent") -ge 1) 'emulator sent its chat (seen on Hookshot: shots hs\*-chat-sent / after, overlay)'
Row 'chat-hookA-sent' ((Count $hsLog['A'] "coop: test chat 'hello from hookshot A': sent") -ge 1) 'HookA sent its chat (seen in the emulator: shot emu\*after*)'
Row 'chat-hookB-sent' ((Count $hsLog['B'] "coop: test chat 'hello from hookshot B': sent") -ge 1) 'HookB sent its chat'
foreach ($x in 'A', 'B') { Row "item-emu-to-hook$x" ((Count $hsLog[$x] 'coop: notice: A got ') -ge 1) "Hook$x log: 'coop: notice: A got ...' (the emulator's give bombs)" }
Row 'item-hookA-to-emu' ((Count $emuLog 'coop: notice: HookA got ') -ge 1) "emulator log: 'coop: notice: HookA got ...' (HookA's give bow)"
Row 'item-hookA-to-hookB' ((Count $hsLog['B'] 'coop: notice: HookA got ') -ge 1) "HookB log: 'coop: notice: HookA got ...'"
$crash = (Count $emuLog 'CRASH') + (Count $hsLog['A'] 'CRASH') + (Count $hsLog['B'] 'CRASH')
Row 'no-crash' ($crash -eq 0) "$crash CRASH lines in the three logs"
foreach ($d in @($emuDir, $hsDir)) {
    if ($d -and (Test-Path (Join-Path $d.FullName 'FAILED.txt'))) { Row "run-$($d.Parent.Name)" $false (Get-Content -Raw (Join-Path $d.FullName 'FAILED.txt')).Trim() }
}
if (-not $emuDir) { Row 'run-emu' $false 'no emulator run folder (see emu-out.txt / emu-err.txt)' }
if (-not $hsDir) { Row 'run-hs' $false 'no Hookshot run folder (see hs-out.txt / hs-err.txt)' }
Set-Content -Encoding utf8 "$out\checks.txt" $rows
$rows | ForEach-Object { Write-Host $_ }
$bad = @($rows | Where-Object { $_ -like 'FAIL *' }).Count
if ($bad) { Set-Content "$out\FAILED.txt" "$bad check(s) failed: see checks.txt" }
$out
if ($bad) { exit 1 }
